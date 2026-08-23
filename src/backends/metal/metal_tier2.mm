#include "backends/metal/metal_tier2.h"

#include "compiler/compiler.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace vg::metal::tier2 {
namespace {

std::vector<uint32_t> g_last_selected_classes;

// Default E010 path: GPU buckets published tasks by authorized node
// class, writes per-class indirect dispatch args, then issues one
// indirect dispatch per class. The host does not read the bucket
// counts before those dispatches run -- a post-wait readback is
// verification only (same pattern as last_tier1_indirect_dims).
//
// ICB is deliberately unused. M1 concurrent-dispatch ICB command types
// are not a hard gate (ADR-021/026: ICB is Tier2's *upgrade*, not
// Tier1 and not this milestone's floor). Classifying this path as
// DevicePass would pretend a native GPU-driven select ran.
constexpr uint32_t kMaxAuthorizedClasses = 16;

const char* kTier2MetalSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VgTier2Params {
  uint task_count;
  uint class_count;
  uint words_per_record;
};

kernel void vg_tier2_bucket(device const uint* fields [[buffer(0)]],
                            device const uint* authorized [[buffer(1)]],
                            device atomic_uint* bucket_counts [[buffer(2)]],
                            device uint* bucket_indices [[buffer(3)]],
                            device uint* selected_classes [[buffer(4)]],
                            device atomic_uint* unauthorized_flag [[buffer(5)]],
                            constant VgTier2Params& params [[buffer(6)]],
                            uint gid [[thread_position_in_grid]]) {
  if (gid >= params.task_count) return;
  uint node = fields[gid * params.words_per_record];
  uint found = 0xffffffffu;
  for (uint c = 0u; c < params.class_count; ++c) {
    if (authorized[c] == node) {
      found = c;
      break;
    }
  }
  selected_classes[gid] = node;
  if (found == 0xffffffffu) {
    atomic_store_explicit(unauthorized_flag, 1u, memory_order_relaxed);
    return;
  }
  uint slot = atomic_fetch_add_explicit(bucket_counts + found, 1u, memory_order_relaxed);
  bucket_indices[found * params.task_count + slot] = gid;
}

kernel void vg_tier2_fill_indirect(device const atomic_uint* bucket_counts [[buffer(0)]],
                                   device uint* indirect_args [[buffer(1)]],
                                   uint gid [[thread_position_in_grid]]) {
  uint count = atomic_load_explicit(bucket_counts + gid, memory_order_relaxed);
  indirect_args[gid * 3u + 0u] = count;
  indirect_args[gid * 3u + 1u] = 1u;
  indirect_args[gid * 3u + 2u] = 1u;
}

kernel void vg_tier2_noop(device const uint* bucket_indices [[buffer(0)]],
                          uint gid [[thread_position_in_grid]]) {
  (void)bucket_indices;
  (void)gid;
}
)MSL";

struct Pipelines {
  id<MTLDevice> device = nil;
  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> bucket = nil;
  id<MTLComputePipelineState> fill = nil;
  id<MTLComputePipelineState> noop = nil;
};

Pipelines& cached_pipelines() {
  static Pipelines pipelines;
  return pipelines;
}

bool ensure_pipelines(id<MTLDevice> device, std::string* error) {
  Pipelines& cache = cached_pipelines();
  if (cache.bucket != nil && cache.device == device) return true;
  cache = Pipelines{};
  cache.device = device;

  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  id<MTLLibrary> library =
      [device newLibraryWithSource:[NSString stringWithUTF8String:kTier2MetalSource]
                           options:options
                             error:&compile_error];
  if (library == nil) {
    if (error)
      *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                    : "unknown Metal Tier2 MSL compile error";
    return false;
  }

  const auto make_pipeline = [&](NSString* name, id<MTLComputePipelineState>* out) -> bool {
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (function == nil) {
      if (error) *error = "Metal Tier2 MSL library missing entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                 error:&pipeline_error];
    if (pipeline == nil) {
      if (error)
        *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                       : "unknown Metal Tier2 pipeline creation error";
      return false;
    }
    *out = pipeline;
    return true;
  };

  if (!make_pipeline(@"vg_tier2_bucket", &cache.bucket)) return false;
  if (!make_pipeline(@"vg_tier2_fill_indirect", &cache.fill)) return false;
  if (!make_pipeline(@"vg_tier2_noop", &cache.noop)) return false;
  cache.library = library;
  return true;
}

id<MTLBuffer> make_shared(id<MTLDevice> device, size_t bytes) {
  return [device newBufferWithLength:std::max<size_t>(bytes, 1) options:MTLResourceStorageModeShared];
}

}  // namespace

const std::vector<uint32_t>& last_selected_node_classes() { return g_last_selected_classes; }

bool apply_select(void* mtl_device, void* mtl_command_queue, void* mtl_fields_buffer,
                  uint32_t task_count, const hal::ExecutionPlan& plan,
                  hal::Submission* submission, uint64_t* encoder_count,
                  uint64_t* command_buffer_count, uint64_t* queue_wait_count,
                  std::string* error) {
  g_last_selected_classes.clear();
  id<MTLDevice> device = static_cast<id<MTLDevice>>(mtl_device);
  id<MTLCommandQueue> command_queue = static_cast<id<MTLCommandQueue>>(mtl_command_queue);
  id<MTLBuffer> fields_buffer = static_cast<id<MTLBuffer>>(mtl_fields_buffer);
  if (device == nil || command_queue == nil || fields_buffer == nil) {
    if (error) *error = "Metal Tier2 select is missing a device, queue, or fields buffer";
    return false;
  }
  if (!plan.request_tier2_select) {
    if (error) *error = "Metal Tier2 select invoked without request_tier2_select";
    return false;
  }
  const uint32_t class_count = static_cast<uint32_t>(plan.authorized_node_classes.size());
  if (class_count < 2 || class_count > kMaxAuthorizedClasses || task_count == 0) {
    if (error) *error = "Metal Tier2 select requires 2..16 authorized classes and a non-empty task graph";
    return false;
  }
  if (!ensure_pipelines(device, error)) return false;
  const Pipelines& pipelines = cached_pipelines();

  const size_t authorized_bytes = class_count * sizeof(uint32_t);
  const size_t counts_bytes = class_count * sizeof(uint32_t);
  const size_t indices_bytes = static_cast<size_t>(class_count) * task_count * sizeof(uint32_t);
  const size_t selected_bytes = static_cast<size_t>(task_count) * sizeof(uint32_t);
  const size_t flag_bytes = sizeof(uint32_t);
  const size_t indirect_bytes = class_count * 3 * sizeof(uint32_t);
  const size_t params_bytes = 3 * sizeof(uint32_t);
  const uint64_t temporary_bytes = authorized_bytes + counts_bytes + indices_bytes + selected_bytes +
                                   flag_bytes + indirect_bytes + params_bytes;

  id<MTLBuffer> authorized_buffer = make_shared(device, authorized_bytes);
  id<MTLBuffer> counts_buffer = make_shared(device, counts_bytes);
  id<MTLBuffer> indices_buffer = make_shared(device, indices_bytes);
  id<MTLBuffer> selected_buffer = make_shared(device, selected_bytes);
  id<MTLBuffer> flag_buffer = make_shared(device, flag_bytes);
  id<MTLBuffer> indirect_buffer = make_shared(device, indirect_bytes);
  id<MTLBuffer> params_buffer = make_shared(device, params_bytes);
  if (authorized_buffer == nil || counts_buffer == nil || indices_buffer == nil ||
      selected_buffer == nil || flag_buffer == nil || indirect_buffer == nil || params_buffer == nil) {
    if (error) *error = "Metal Tier2 buffer allocation failed";
    return false;
  }

  std::memcpy([authorized_buffer contents], plan.authorized_node_classes.data(), authorized_bytes);
  std::memset([counts_buffer contents], 0, counts_bytes);
  std::memset([indices_buffer contents], 0, indices_bytes);
  std::memset([selected_buffer contents], 0, selected_bytes);
  std::memset([flag_buffer contents], 0, flag_bytes);
  uint32_t* params = static_cast<uint32_t*>([params_buffer contents]);
  params[0] = task_count;
  params[1] = class_count;
  params[2] = compiler::kTaskRingWordsPerRecord;

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) {
    if (error) *error = "failed to create Metal Tier2 command buffer";
    return false;
  }

  id<MTLComputeCommandEncoder> bucket_encoder = [command_buffer computeCommandEncoder];
  if (bucket_encoder == nil) {
    if (error) *error = "failed to create Metal Tier2 bucket encoder";
    return false;
  }
  [bucket_encoder setComputePipelineState:pipelines.bucket];
  [bucket_encoder setBuffer:fields_buffer offset:0 atIndex:0];
  [bucket_encoder setBuffer:authorized_buffer offset:0 atIndex:1];
  [bucket_encoder setBuffer:counts_buffer offset:0 atIndex:2];
  [bucket_encoder setBuffer:indices_buffer offset:0 atIndex:3];
  [bucket_encoder setBuffer:selected_buffer offset:0 atIndex:4];
  [bucket_encoder setBuffer:flag_buffer offset:0 atIndex:5];
  [bucket_encoder setBuffer:params_buffer offset:0 atIndex:6];
  [bucket_encoder dispatchThreadgroups:MTLSizeMake(task_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [bucket_encoder endEncoding];

  id<MTLComputeCommandEncoder> fill_encoder = [command_buffer computeCommandEncoder];
  if (fill_encoder == nil) {
    if (error) *error = "failed to create Metal Tier2 fill-indirect encoder";
    return false;
  }
  [fill_encoder setComputePipelineState:pipelines.fill];
  [fill_encoder setBuffer:counts_buffer offset:0 atIndex:0];
  [fill_encoder setBuffer:indirect_buffer offset:0 atIndex:1];
  [fill_encoder dispatchThreadgroups:MTLSizeMake(class_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [fill_encoder endEncoding];

  // Per-Node indirect: GPU-authored bucket counts drive dispatch. The
  // host has not read those counts. Same-CB automatic hazard tracking
  // makes the fill visible to the indirect argument fetch.
  id<MTLComputeCommandEncoder> dispatch_encoder = [command_buffer computeCommandEncoder];
  if (dispatch_encoder == nil) {
    if (error) *error = "failed to create Metal Tier2 per-node encoder";
    return false;
  }
  [dispatch_encoder setComputePipelineState:pipelines.noop];
  [dispatch_encoder setBuffer:indices_buffer offset:0 atIndex:0];
  for (uint32_t node = 0; node < class_count; ++node) {
    [dispatch_encoder dispatchThreadgroupsWithIndirectBuffer:indirect_buffer
                                        indirectBufferOffset:node * 3 * sizeof(uint32_t)
                                       threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  }
  [dispatch_encoder endEncoding];

  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (encoder_count != nullptr) *encoder_count += 3;
  if (command_buffer_count != nullptr) *command_buffer_count += 1;
  if (queue_wait_count != nullptr) *queue_wait_count += 1;

  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                           : "Metal Tier2 select dispatch failed";
    return false;
  }

  const uint32_t* selected = static_cast<const uint32_t*>([selected_buffer contents]);
  g_last_selected_classes.assign(selected, selected + task_count);

  const uint32_t unauthorized = *static_cast<const uint32_t*>([flag_buffer contents]);
  if (unauthorized != 0) {
    if (error) *error = "tier2 select refused unauthorized node class";
    if (submission != nullptr) {
      submission->report.add("tier2_node_select", hal::LoweringClass::Unsupported, 1, temporary_bytes,
                             "GPU bucket kernel rejected a node class outside the authorized envelope; "
                             "Tier3 (GPU invents a new Node) remains Unsupported");
    }
    return false;
  }

  if (submission != nullptr) {
    submission->report.add("tier2_node_select", hal::LoweringClass::EmulatedDevicePass, class_count,
                           temporary_bytes,
                           "bucket compute + per-node indirect; ICB unused (optional upgrade, not required)");
    submission->report.add("tier2_bucket_count", hal::LoweringClass::EmulatedDevicePass, class_count, 0,
                           "one GPU bucket per authorized node class");
    submission->report.add("tier2_pipeline_switch", hal::LoweringClass::EmulatedDevicePass, class_count, 0,
                           "one compute PSO bind covering every authorized node class's indirect dispatch");
  }
  return true;
}

}  // namespace vg::metal::tier2
