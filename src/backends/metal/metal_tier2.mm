#include "backends/metal/metal_tier2.h"

#include "compiler/compute_task_ring.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg::metal::tier2 {
namespace {

std::vector<uint32_t> g_last_selected_classes;

constexpr uint32_t kMaxAuthorizedClasses = 16;
constexpr uint32_t kMaxIcbCommands = 16384;
constexpr uint32_t kIcbKernelBufferCount = 2;

const char* kTier2BucketSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VgTier2Params {
  uint task_count;
  uint class_count;
  uint words_per_record;
  uint node_word;
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
  uint node = fields[gid * params.words_per_record + params.node_word];
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

const char* kTier2IcbSource = R"MSL(
#include <metal_stdlib>
#include <metal_command_buffer>
using namespace metal;

struct VgTier2Params {
  uint task_count;
  uint class_count;
  uint words_per_record;
  uint node_word;
};

struct VgIcbContainer {
  command_buffer icb [[id(0)]];
  array<compute_pipeline_state, 16> pipelines [[id(1)]];
};

constant uint kIcbNodeClass [[function_constant(0)]];

kernel void vg_tier2_icb_node(device uint* selected [[buffer(0)]],
                              device const uint* task_index [[buffer(1)]]) {
  selected[task_index[0]] = kIcbNodeClass;
}

kernel void vg_tier2_icb_encode(device const uint* fields [[buffer(0)]],
                                device const uint* authorized [[buffer(1)]],
                                device atomic_uint* unauthorized_flag [[buffer(2)]],
                                constant VgTier2Params& params [[buffer(3)]],
                                device VgIcbContainer* container [[buffer(4)]],
                                device uint* selected [[buffer(5)]],
                                device const uint* slot_indices [[buffer(6)]],
                                uint gid [[thread_position_in_grid]]) {
  if (gid >= params.task_count) return;
  compute_command cmd(container->icb, gid);
  cmd.reset();
  uint node = fields[gid * params.words_per_record + params.node_word];
  uint found = 0xffffffffu;
  for (uint c = 0u; c < params.class_count; ++c) {
    if (authorized[c] == node) {
      found = c;
      break;
    }
  }
  if (found == 0xffffffffu) {
    atomic_store_explicit(unauthorized_flag, 1u, memory_order_relaxed);
    return;
  }
  cmd.set_compute_pipeline_state(container->pipelines[found]);
  cmd.set_kernel_buffer(selected, 0);
  cmd.set_kernel_buffer(slot_indices + gid, 1);
  cmd.concurrent_dispatch_threadgroups(uint3(1u, 1u, 1u), uint3(1u, 1u, 1u));
}
)MSL";

struct Pipelines {
  id<MTLDevice> device = nil;
  id<MTLLibrary> bucket_library = nil;
  id<MTLLibrary> icb_library = nil;
  id<MTLComputePipelineState> bucket = nil;
  id<MTLComputePipelineState> fill = nil;
  id<MTLComputePipelineState> noop = nil;
  id<MTLComputePipelineState> icb_encode = nil;
  std::unordered_map<uint32_t, id<MTLComputePipelineState>> icb_nodes;
};

Pipelines& cached_pipelines() {
  static Pipelines pipelines;
  return pipelines;
}

id<MTLBuffer> make_shared(id<MTLDevice> device, size_t bytes) {
  return [device newBufferWithLength:std::max<size_t>(bytes, 1) options:MTLResourceStorageModeShared];
}

bool argument_buffers_usable(id<MTLDevice> device) {
  if (![device respondsToSelector:@selector(argumentBuffersSupport)]) return false;
  return [device argumentBuffersSupport] >= MTLArgumentBuffersTier2;
}

id<MTLLibrary> compile_library(id<MTLDevice> device, const char* source, std::string* error,
                               MTLLanguageVersion language_version) {
  MTLCompileOptions* options = [MTLCompileOptions new];
  options.languageVersion = language_version;
  NSError* compile_error = nil;
  id<MTLLibrary> library =
      [device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&compile_error];
  if (library == nil) {
    if (error)
      *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                    : "unknown Metal Tier2 MSL compile error";
    return nil;
  }
  return library;
}

void reset_cache_if_device_changed(id<MTLDevice> device) {
  Pipelines& cache = cached_pipelines();
  if (cache.device != nil && cache.device != device) cache = Pipelines{};
  cache.device = device;
}

bool ensure_bucket_library(id<MTLDevice> device, std::string* error) {
  reset_cache_if_device_changed(device);
  Pipelines& cache = cached_pipelines();
  if (cache.bucket_library != nil) return true;
  cache.bucket_library = compile_library(device, kTier2BucketSource, error, MTLLanguageVersion2_2);
  return cache.bucket_library != nil;
}

bool ensure_icb_library(id<MTLDevice> device, std::string* error) {
  reset_cache_if_device_changed(device);
  Pipelines& cache = cached_pipelines();
  if (cache.icb_library != nil) return true;
  cache.icb_library = compile_library(device, kTier2IcbSource, error, MTLLanguageVersion3_0);
  return cache.icb_library != nil;
}

bool make_plain_pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString* name,
                         id<MTLComputePipelineState>* out, std::string* error) {
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
}

bool ensure_bucket_pipelines(id<MTLDevice> device, std::string* error) {
  if (!ensure_bucket_library(device, error)) return false;
  Pipelines& cache = cached_pipelines();
  if (cache.bucket != nil) return true;
  if (!make_plain_pipeline(device, cache.bucket_library, @"vg_tier2_bucket", &cache.bucket, error))
    return false;
  if (!make_plain_pipeline(device, cache.bucket_library, @"vg_tier2_fill_indirect", &cache.fill, error))
    return false;
  if (!make_plain_pipeline(device, cache.bucket_library, @"vg_tier2_noop", &cache.noop, error))
    return false;
  return true;
}

bool ensure_icb_encode_pipeline(id<MTLDevice> device, std::string* error) {
  if (!ensure_icb_library(device, error)) return false;
  Pipelines& cache = cached_pipelines();
  if (cache.icb_encode != nil) return true;
  return make_plain_pipeline(device, cache.icb_library, @"vg_tier2_icb_encode", &cache.icb_encode, error);
}

id<MTLComputePipelineState> icb_node_pipeline(id<MTLDevice> device, uint32_t node_class,
                                              std::string* error) {
  if (!ensure_icb_library(device, error)) return nil;
  Pipelines& cache = cached_pipelines();
  auto it = cache.icb_nodes.find(node_class);
  if (it != cache.icb_nodes.end()) return it->second;

  MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
  [constants setConstantValue:&node_class type:MTLDataTypeUInt atIndex:0];
  NSError* function_error = nil;
  id<MTLFunction> function = [cache.icb_library newFunctionWithName:@"vg_tier2_icb_node"
                                                 constantValues:constants
                                                          error:&function_error];
  if (function == nil) {
    if (error)
      *error = function_error != nil ? [[function_error localizedDescription] UTF8String]
                                     : "Metal Tier2 ICB node function specialize failed";
    return nil;
  }
  MTLComputePipelineDescriptor* descriptor = [MTLComputePipelineDescriptor new];
  descriptor.computeFunction = function;
  descriptor.supportIndirectCommandBuffers = YES;
  NSError* pipeline_error = nil;
  id<MTLComputePipelineState> pipeline =
      [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone
                                        reflection:nil error:&pipeline_error];
  if (pipeline == nil) {
    if (error)
      *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                     : "Metal Tier2 ICB node pipeline creation failed";
    return nil;
  }
  cache.icb_nodes.emplace(node_class, pipeline);
  return pipeline;
}

void bump_counters(uint64_t encoders, DispatchCounters counters) {
  if (counters.encoder_count != nullptr) *counters.encoder_count += encoders;
  if (counters.command_buffer_count != nullptr) *counters.command_buffer_count += 1;
  if (counters.queue_wait_count != nullptr) *counters.queue_wait_count += 1;
}

enum class IcbAttempt { Ok, Unauthorized, Unavailable };

IcbAttempt icb_unavailable(std::string* error, const char* message) {
  if (error != nullptr && message != nullptr) *error = message;
  return IcbAttempt::Unavailable;
}

bool finish_command_buffer(id<MTLCommandBuffer> command_buffer, std::string* error) {
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                           : "Metal Tier2 command buffer failed";
    return false;
  }
  return true;
}

IcbAttempt apply_icb_select(id<MTLDevice> device, id<MTLCommandQueue> command_queue,
                            id<MTLBuffer> fields_buffer, uint32_t task_count,
                            const std::vector<uint32_t>& authorized_node_classes,
                            hal::Submission* submission,
                            DispatchCounters counters, std::string* error) {
  if (task_count == 0 || task_count > kMaxIcbCommands)
    return icb_unavailable(error, "ICB path requires 1..16384 published tasks");
  if (!argument_buffers_usable(device))
    return icb_unavailable(error, "argumentBuffersSupport is below Tier2");
  if (!ensure_icb_encode_pipeline(device, error))
    return IcbAttempt::Unavailable;

  const auto class_count = static_cast<uint32_t>(authorized_node_classes.size());
  std::vector<id<MTLComputePipelineState>> node_pipelines(class_count, nil);
  for (uint32_t i = 0; i < class_count; ++i) {
    node_pipelines[i] = icb_node_pipeline(device, authorized_node_classes[i], error);
    if (node_pipelines[i] == nil) return IcbAttempt::Unavailable;
  }

  Pipelines& cache = cached_pipelines();
  id<MTLFunction> encode_function = [cache.icb_library newFunctionWithName:@"vg_tier2_icb_encode"];
  if (encode_function == nil) return icb_unavailable(error, "ICB encode function missing after compile");
  id<MTLArgumentEncoder> arg_encoder = [encode_function newArgumentEncoderWithBufferIndex:4];
  if (arg_encoder == nil)
    return icb_unavailable(error, "newArgumentEncoderWithBufferIndex:4 returned nil");

  MTLIndirectCommandBufferDescriptor* icb_desc = [MTLIndirectCommandBufferDescriptor new];
  icb_desc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
  icb_desc.inheritPipelineState = NO;
  icb_desc.inheritBuffers = NO;
  icb_desc.maxKernelBufferBindCount = kIcbKernelBufferCount;
  id<MTLIndirectCommandBuffer> icb =
      [device newIndirectCommandBufferWithDescriptor:icb_desc
                                     maxCommandCount:task_count
                                             options:MTLResourceStorageModePrivate];
  if (icb == nil) return icb_unavailable(error, "newIndirectCommandBuffer returned nil");

  const size_t authorized_bytes = class_count * sizeof(uint32_t);
  const size_t selected_bytes = static_cast<size_t>(task_count) * sizeof(uint32_t);
  const size_t flag_bytes = sizeof(uint32_t);
  const size_t params_bytes = 4 * sizeof(uint32_t);
  const size_t slot_bytes = static_cast<size_t>(task_count) * sizeof(uint32_t);
  const size_t arg_bytes = [arg_encoder encodedLength];
  const uint64_t temporary_bytes =
      authorized_bytes + selected_bytes + flag_bytes + params_bytes + slot_bytes + arg_bytes;

  id<MTLBuffer> authorized_buffer = make_shared(device, authorized_bytes);
  id<MTLBuffer> selected_buffer = make_shared(device, selected_bytes);
  id<MTLBuffer> flag_buffer = make_shared(device, flag_bytes);
  id<MTLBuffer> params_buffer = make_shared(device, params_bytes);
  id<MTLBuffer> slot_buffer = make_shared(device, slot_bytes);
  id<MTLBuffer> arg_buffer = [device newBufferWithLength:std::max<size_t>(arg_bytes, 1)
                                                 options:MTLResourceStorageModeShared];
  if (authorized_buffer == nil || selected_buffer == nil || flag_buffer == nil ||
      params_buffer == nil || slot_buffer == nil || arg_buffer == nil) {
    return icb_unavailable(error, "ICB temporary buffer allocation failed");
  }
  if ([arg_encoder encodedLength] == 0)
    return icb_unavailable(error, "ICB argument encoder encodedLength is 0");

  std::memcpy([authorized_buffer contents], authorized_node_classes.data(), authorized_bytes);
  std::memset([selected_buffer contents], 0, selected_bytes);
  std::memset([flag_buffer contents], 0, flag_bytes);
  auto* params = static_cast<uint32_t*>([params_buffer contents]);
  params[0] = task_count;
  params[1] = class_count;
  params[2] = compiler::kTaskRingWordsPerRecord;
  params[3] = compiler::kTaskRingNodeIndexWord;
  auto* slots = static_cast<uint32_t*>([slot_buffer contents]);
  for (uint32_t i = 0; i < task_count; ++i) slots[i] = i;

  [arg_encoder setArgumentBuffer:arg_buffer offset:0];
  [arg_encoder setIndirectCommandBuffer:icb atIndex:0];
  for (uint32_t i = 0; i < class_count; ++i)
    [arg_encoder setComputePipelineState:node_pipelines[i] atIndex:1 + i];
  for (uint32_t i = class_count; i < kMaxAuthorizedClasses; ++i)
    [arg_encoder setComputePipelineState:node_pipelines[0] atIndex:1 + i];

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) return icb_unavailable(error, "failed to create ICB command buffer");

  uint64_t encoders = 0;
  id<MTLBlitCommandEncoder> reset = [command_buffer blitCommandEncoder];
  if (reset == nil) return icb_unavailable(error, "failed to create ICB reset blit encoder");
  [reset resetCommandsInBuffer:icb withRange:NSMakeRange(0, task_count)];
  [reset endEncoding];
  ++encoders;

  id<MTLComputeCommandEncoder> encode = [command_buffer computeCommandEncoder];
  if (encode == nil) return icb_unavailable(error, "failed to create ICB encode compute encoder");
  [encode setComputePipelineState:cache.icb_encode];
  [encode setBuffer:fields_buffer offset:0 atIndex:0];
  [encode setBuffer:authorized_buffer offset:0 atIndex:1];
  [encode setBuffer:flag_buffer offset:0 atIndex:2];
  [encode setBuffer:params_buffer offset:0 atIndex:3];
  [encode setBuffer:arg_buffer offset:0 atIndex:4];
  [encode setBuffer:selected_buffer offset:0 atIndex:5];
  [encode setBuffer:slot_buffer offset:0 atIndex:6];
  [encode useResource:icb usage:MTLResourceUsageWrite];
  [encode useResource:selected_buffer usage:MTLResourceUsageWrite];
  [encode useResource:slot_buffer usage:MTLResourceUsageRead];
  [encode dispatchThreadgroups:MTLSizeMake(task_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encode endEncoding];
  ++encoders;

  id<MTLBlitCommandEncoder> optimize = [command_buffer blitCommandEncoder];
  if (optimize != nil) {
    [optimize optimizeIndirectCommandBuffer:icb withRange:NSMakeRange(0, task_count)];
    [optimize endEncoding];
    ++encoders;
  }

  id<MTLComputeCommandEncoder> execute = [command_buffer computeCommandEncoder];
  if (execute == nil) return icb_unavailable(error, "failed to create ICB execute encoder");
  [execute useResource:icb usage:MTLResourceUsageRead];
  [execute useResource:selected_buffer usage:MTLResourceUsageWrite];
  [execute useResource:slot_buffer usage:MTLResourceUsageRead];
  [execute executeCommandsInBuffer:icb withRange:NSMakeRange(0, task_count)];
  [execute endEncoding];
  ++encoders;

  if (!finish_command_buffer(command_buffer, error)) return IcbAttempt::Unavailable;
  bump_counters(encoders, counters);

  const auto* selected = static_cast<const uint32_t*>([selected_buffer contents]);
  g_last_selected_classes.assign(selected, selected + task_count);
  const uint32_t unauthorized = *static_cast<const uint32_t*>([flag_buffer contents]);
  if (unauthorized != 0) {
    if (error) *error = "tier2 select refused unauthorized node class";
    if (submission != nullptr) {
      submission->report.add("tier2_node_select", hal::LoweringClass::Unsupported, 1, temporary_bytes,
                             "GPU ICB encode rejected a node class outside the authorized set; "
                             "Tier3 (GPU invents a new pipeline) remains Unsupported");
    }
    return IcbAttempt::Unauthorized;
  }

  if (submission != nullptr) {
    submission->report.add("tier2_node_select", hal::LoweringClass::DevicePass, class_count,
                           temporary_bytes,
                           "GPU-encoded ICB: one compute command per task, distinct pre-authorized "
                           "PSO per node class, executeCommandsInBuffer, no host count readback");
    submission->report.add("tier2_icb_execute", hal::LoweringClass::DevicePass, task_count, 0,
                           "executeCommandsInBuffer after reset and optimize; host range is the "
                           "sealed task count, not a read-back histogram");
    submission->report.add("tier2_pipeline_switch", hal::LoweringClass::DevicePass, class_count, 0,
                           "inheritPipelineState=NO; one ICB-capable compute PSO per authorized class");
  }
  return IcbAttempt::Ok;
}

bool apply_bucket_select(id<MTLDevice> device, id<MTLCommandQueue> command_queue,
                         id<MTLBuffer> fields_buffer, uint32_t task_count,
                         const std::vector<uint32_t>& authorized_node_classes,
                         hal::Submission* submission,
                         DispatchCounters counters, const std::string& fallback_reason,
                         std::string* error) {
  if (!ensure_bucket_pipelines(device, error)) return false;
  const Pipelines& pipelines = cached_pipelines();
  const auto class_count = static_cast<uint32_t>(authorized_node_classes.size());

  const size_t authorized_bytes = static_cast<size_t>(class_count) * sizeof(uint32_t);
  const size_t counts_bytes = static_cast<size_t>(class_count) * sizeof(uint32_t);
  const size_t indices_bytes = static_cast<size_t>(class_count) * static_cast<size_t>(task_count) *
                               sizeof(uint32_t);
  const size_t selected_bytes = static_cast<size_t>(task_count) * sizeof(uint32_t);
  const size_t flag_bytes = sizeof(uint32_t);
  const size_t indirect_bytes = static_cast<size_t>(class_count) * 3 * sizeof(uint32_t);
  const size_t params_bytes = 4 * sizeof(uint32_t);
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

  std::memcpy([authorized_buffer contents], authorized_node_classes.data(), authorized_bytes);
  std::memset([counts_buffer contents], 0, counts_bytes);
  std::memset([indices_buffer contents], 0, indices_bytes);
  std::memset([selected_buffer contents], 0, selected_bytes);
  std::memset([flag_buffer contents], 0, flag_bytes);
  auto* params = static_cast<uint32_t*>([params_buffer contents]);
  params[0] = task_count;
  params[1] = class_count;
  params[2] = compiler::kTaskRingWordsPerRecord;
  params[3] = compiler::kTaskRingNodeIndexWord;

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
  [bucket_encoder dispatchThreadgroups:MTLSizeMake(task_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [bucket_encoder endEncoding];

  id<MTLComputeCommandEncoder> fill_encoder = [command_buffer computeCommandEncoder];
  if (fill_encoder == nil) {
    if (error) *error = "failed to create Metal Tier2 fill-indirect encoder";
    return false;
  }
  [fill_encoder setComputePipelineState:pipelines.fill];
  [fill_encoder setBuffer:counts_buffer offset:0 atIndex:0];
  [fill_encoder setBuffer:indirect_buffer offset:0 atIndex:1];
  [fill_encoder dispatchThreadgroups:MTLSizeMake(class_count, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [fill_encoder endEncoding];

  id<MTLComputeCommandEncoder> dispatch_encoder = [command_buffer computeCommandEncoder];
  if (dispatch_encoder == nil) {
    if (error) *error = "failed to create Metal Tier2 per-node encoder";
    return false;
  }
  [dispatch_encoder setComputePipelineState:pipelines.noop];
  [dispatch_encoder setBuffer:indices_buffer offset:0 atIndex:0];
  for (uint32_t node = 0; node < class_count; ++node) {
    [dispatch_encoder dispatchThreadgroupsWithIndirectBuffer:indirect_buffer
                                        indirectBufferOffset:static_cast<NSUInteger>(node) * 3 *
                                                             sizeof(uint32_t)
                                       threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  }
  [dispatch_encoder endEncoding];

  if (!finish_command_buffer(command_buffer, error)) return false;
  bump_counters(3, counters);

  const auto* selected = static_cast<const uint32_t*>([selected_buffer contents]);
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
    const std::string reason =
        fallback_reason.empty()
            ? std::string("bucket compute + per-node indirect; ICB unavailable")
            : ("bucket compute + per-node indirect; ICB fallback: " + fallback_reason);
    submission->report.add("tier2_node_select", hal::LoweringClass::EmulatedDevicePass, class_count,
                           temporary_bytes, reason);
    submission->report.add("tier2_bucket_count", hal::LoweringClass::EmulatedDevicePass, class_count, 0,
                           "one GPU bucket per authorized node class");
    submission->report.add("tier2_pipeline_switch", hal::LoweringClass::EmulatedDevicePass, class_count,
                           0, "one compute PSO bind covering every authorized node class's indirect dispatch");
  }
  return true;
}

}  // namespace

const std::vector<uint32_t>& last_selected_node_classes() { return g_last_selected_classes; }

bool apply_select(const MetalSelectContext& metal, uint32_t task_count,
                  const std::vector<uint32_t>& authorized_node_classes,
                  hal::Submission* submission, DispatchCounters counters, std::string* error) {
  g_last_selected_classes.clear();
  auto device = static_cast<id<MTLDevice>>(metal.device);
  auto command_queue = static_cast<id<MTLCommandQueue>>(metal.command_queue);
  auto fields_buffer = static_cast<id<MTLBuffer>>(metal.fields_buffer);
  if (device == nil || command_queue == nil || fields_buffer == nil) {
    if (error) *error = "Metal Tier2 select is missing a device, queue, or fields buffer";
    return false;
  }
  const auto class_count = static_cast<uint32_t>(authorized_node_classes.size());
  if (class_count < 2 || class_count > kMaxAuthorizedClasses || task_count == 0) {
    if (error) *error = "Metal Tier2 select requires 2..16 authorized classes and a non-empty task graph";
    return false;
  }

  std::string icb_error;
  const IcbAttempt icb = apply_icb_select(device, command_queue, fields_buffer, task_count,
                                          authorized_node_classes,
                                          submission, counters, &icb_error);
  if (icb == IcbAttempt::Ok) return true;
  if (icb == IcbAttempt::Unauthorized) {
    if (error) *error = icb_error;
    return false;
  }
  return apply_bucket_select(device, command_queue, fields_buffer, task_count,
                             authorized_node_classes, submission,
                             counters, icb_error, error);
}

bool run_select_test_harness(const std::vector<uint32_t>& task_node_classes,
                             const std::vector<uint32_t>& authorized_node_classes,
                             hal::Submission* submission, std::string* error) {
  if (submission == nullptr) {
    if (error) *error = "Tier2 test harness requires a submission output";
    return false;
  }
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = device != nil ? [device newCommandQueue] : nil;
  if (device == nil || queue == nil) {
    if (error) *error = "Tier2 test harness requires a Metal device and command queue";
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(task_node_classes.size());
  id<MTLBuffer> fields = [device newBufferWithLength:std::max<size_t>(
      static_cast<size_t>(count) * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                               options:MTLResourceStorageModeShared];
  if (fields == nil) {
    if (error) *error = "Tier2 test harness fields-buffer allocation failed";
    return false;
  }
  auto* words = static_cast<uint32_t*>([fields contents]);
  std::memset(words, 0, static_cast<size_t>(count) * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t));
  for (uint32_t i = 0; i < count; ++i)
    words[static_cast<size_t>(i) * compiler::kTaskRingWordsPerRecord +
          compiler::kTaskRingNodeIndexWord] = task_node_classes[i];
  const bool selected = apply_select({.device = static_cast<void*>(device), .command_queue = static_cast<void*>(queue),
                                      .fields_buffer = static_cast<void*>(fields)},
                                     count, authorized_node_classes, submission, {}, error);
  if (selected) submission->result.ok = true;
  return selected;
}

}  // namespace vg::metal::tier2
