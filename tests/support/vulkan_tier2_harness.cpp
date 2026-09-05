#include "vulkan_adapter_harness.h"

#include "backends/vulkan/vulkan_device_internal.h"
#include "compiler/compute_package.h"
#include "compiler/shader_sources.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace vg::vulkan {
namespace {

#if defined(VG_HAS_VULKAN)
using detail::RawBuffer;

bool make_buffer(detail::DeviceState &state, VkDeviceSize bytes,
                 VkBufferUsageFlags usage, bool address, RawBuffer *out,
                 std::string *error) {
  return detail::create_raw_buffer(state.device_, state.physical_device_,
                                   std::max<VkDeviceSize>(bytes, 4),
                                   usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   address, true, out, error);
}

void compute_to(VkCommandBuffer cb, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access) {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cb, &dependency);
}

bool begin(detail::DeviceState &state, VkCommandBuffer *cb,
           std::string *error) {
  if (!detail::ensure_command_pool(state.device_, state.compute_queue_family_,
                                   &state.command_pool_, error) ||
      !detail::allocate_command_buffer(state.device_, state.command_pool_, cb,
                                       error))
    return false;
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(*cb, &info) == VK_SUCCESS)
    return true;
  vkFreeCommandBuffers(state.device_, state.command_pool_, 1, cb);
  *cb = VK_NULL_HANDLE;
  if (error)
    *error = "vkBeginCommandBuffer failed for Vulkan F experiment";
  return false;
}

bool finish(detail::DeviceState &state, VkCommandBuffer cb,
            std::string *error) {
  if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
    vkFreeCommandBuffers(state.device_, state.command_pool_, 1, &cb);
    if (error)
      *error = "vkEndCommandBuffer failed for Vulkan F experiment";
    return false;
  }
  return detail::submit_and_wait_simple(state.device_, state.compute_queue_,
                                        state.command_pool_, cb, error);
}

bool pipeline(detail::DeviceState &state, const char *key,
              const std::string &glsl, uint32_t push_bytes,
              const detail::DeviceState::ComputePipelineRecord **out,
              std::string *error) {
  if (!state.capabilities_.supports(hal::Capability::LinearAddress) ||
      !state.capabilities_.supports(hal::Capability::EffectDag)) {
    if (error)
      *error = "Vulkan F experiments require enabled BDA and synchronization2";
    return false;
  }
  bool ignored_cache_hit = false;
  return state.ensure_pipeline(key, glsl,
                               (push_bytes + sizeof(VkDeviceAddress) - 1) /
                                   sizeof(VkDeviceAddress),
                               out, &ignored_cache_hit, error);
}

template <typename T>
void bind_and_push(VkCommandBuffer cb,
                   const detail::DeviceState::ComputePipelineRecord *p,
                   const T &pc) {
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
  vkCmdPushConstants(cb, p->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(T), &pc);
}

const char *kIndirectWrite = R"GLSL(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in;
layout(buffer_reference, std430, buffer_reference_align=4) buffer Dims { uint d[]; };
layout(buffer_reference, std430, buffer_reference_align=4) buffer Args { uint a[]; };
layout(push_constant) uniform P { Dims dims; Args args; uint count; } p;
void main(){ uint i=gl_GlobalInvocationID.x; if(i<p.count) { p.args.a[i*3]=p.dims.d[i*3]; p.args.a[i*3+1]=p.dims.d[i*3+1]; p.args.a[i*3+2]=p.dims.d[i*3+2]; } }
)GLSL";
const char *kIndirectTarget = R"GLSL(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in;
layout(buffer_reference, std430, buffer_reference_align=4) buffer Count { uint value; };
layout(push_constant) uniform P { Count count; } p;
void main(){ atomicAdd(p.count.value, 1u); }
)GLSL";
const char *kTier2Bucket = R"GLSL(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in;
layout(buffer_reference, std430, buffer_reference_align=4) buffer U32 { uint v[]; };
layout(push_constant) uniform P { U32 nodes; U32 auth; U32 selected; U32 counts; uint task_count; uint auth_count; } p;
void main(){ uint i=gl_GlobalInvocationID.x; if(i>=p.task_count) return; uint n=p.nodes.v[i]; for(uint b=0;b<p.auth_count;++b) if(n==p.auth.v[b]) { p.selected.v[i]=n; atomicAdd(p.counts.v[b],1u); return; } p.selected.v[i]=0xffffffffu; }
)GLSL";
const char *kTier2Fill = R"GLSL(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in;
layout(buffer_reference, std430, buffer_reference_align=4) buffer U32 { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align=4) buffer Args { uint a[]; };
layout(push_constant) uniform P { U32 counts; Args args; uint count; } p;
void main(){ uint i=gl_GlobalInvocationID.x; if(i<p.count) { p.args.a[i*3]=p.counts.v[i]; p.args.a[i*3+1]=1; p.args.a[i*3+2]=1; } }
)GLSL";
const char *kTier2Target = R"GLSL(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in;
layout(buffer_reference, std430, buffer_reference_align=4) buffer Count { uint value; };
layout(push_constant) uniform P { Count count; } p;
void main(){ atomicAdd(p.count.value,1u); }
)GLSL";

#endif
} // namespace

bool AdapterHarness::run_gpu_indirect_experiment(
    const std::vector<std::array<uint32_t, 3>> &dims,
    GpuIndirectExperimentResult *result, std::string *error) const {
  if (result) {
    *result = {};
    result->report.backend = hal::BackendKind::Vulkan;
  }
#if !defined(VG_HAS_VULKAN)
  (void)dims;
  (void)result;
  if (error)
    *error = "Vulkan is unavailable in this build";
  return false;
#else
  if (!result || dims.empty() || dims.size() > 64) {
    if (error)
      *error = "indirect experiment requires 1..64 dimensions";
    return false;
  }
  for (const auto &d : dims)
    if (d[0] == 0 || d[1] != 1 || d[2] != 1 || d[0] > 1024) {
      if (error)
        *error = "indirect dimensions must be bounded x=1..1024,y=z=1";
      return false;
    }
  auto &s = *device_.state_;
  const detail::DeviceState::ComputePipelineRecord *writer{}, *target{};
  if (!pipeline(s, "vulkan-f-indirect-write-v1", kIndirectWrite, 24, &writer,
                error) ||
      !pipeline(s, "vulkan-f-indirect-target-v1", kIndirectTarget, 8, &target,
                error))
    return false;
  RawBuffer input{}, args{}, count{};
  const auto destroy = [&] {
    detail::destroy_raw_buffer(s.device_, &input);
    detail::destroy_raw_buffer(s.device_, &args);
    detail::destroy_raw_buffer(s.device_, &count);
  };
  if (!make_buffer(s, dims.size() * sizeof(dims[0]),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &input, error) ||
      !make_buffer(s, dims.size() * sizeof(VkDispatchIndirectCommand),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                   true, &args, error) ||
      !make_buffer(s, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   true, &count, error)) {
    destroy();
    return false;
  }
  std::memcpy(input.mapped, dims.data(), dims.size() * sizeof(dims[0]));
  std::memset(args.mapped, 0, dims.size() * sizeof(VkDispatchIndirectCommand));
  std::memset(count.mapped, 0, sizeof(uint32_t));
  VkCommandBuffer cb{};
  if (!begin(s, &cb, error)) {
    destroy();
    return false;
  }
  struct WritePc {
    VkDeviceAddress dims, args;
    uint32_t count;
  } write{input.address, args.address, static_cast<uint32_t>(dims.size())};
  bind_and_push(cb, writer, write);
  vkCmdDispatch(cb, static_cast<uint32_t>(dims.size()), 1, 1);
  compute_to(cb, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
  struct TargetPc {
    VkDeviceAddress count;
  } target_pc{count.address};
  bind_and_push(cb, target, target_pc);
  for (size_t i = 0; i < dims.size(); ++i)
    vkCmdDispatchIndirect(cb, args.buffer,
                          i * sizeof(VkDispatchIndirectCommand));
  compute_to(cb, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  if (!finish(s, cb, error)) {
    destroy();
    return false;
  }
  const auto *written = static_cast<const uint32_t *>(args.mapped);
  result->gpu_written_dims.resize(dims.size());
  for (size_t i = 0; i < dims.size(); ++i)
    std::memcpy(result->gpu_written_dims[i].data(), written + i * 3,
                sizeof(result->gpu_written_dims[i]));
  result->gpu_invocation_count = *static_cast<uint32_t *>(count.mapped);
  result->indirect_dispatch_count = static_cast<uint32_t>(dims.size());
  result->report.supported = true;
  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = 2;
  result->report.queue_wait_count = 1;
  result->report.add(
      "indirect_argument_generation", hal::LoweringClass::DevicePass, 1,
      dims.size() * sizeof(VkDispatchIndirectCommand),
      "one direct compute dispatch generated tightly packed indirect records");
  result->report.add(
      "transient_buffers", hal::LoweringClass::Direct, 3, dims.size() * 24 + 4,
      "requested logical transient buffer bytes; excludes allocation padding");
  result->report.add(
      "tier1_indirect_dispatch", hal::LoweringClass::Direct, dims.size(), 0,
      "GPU wrote VkDispatchIndirectCommand records; vkCmdDispatchIndirect "
      "consumed them after a compute-to-indirect barrier");
  destroy();
  return true;
#endif
}

bool AdapterHarness::run_gpu_cull_compact_experiment(
    const std::vector<uint32_t> &visible, const std::vector<uint32_t> &ids,
    GpuCullCompactExperimentResult *result, std::string *error) const {
  if (result) {
    *result = {};
    result->report.backend = hal::BackendKind::Vulkan;
  }
#if !defined(VG_HAS_VULKAN)
  (void)visible;
  (void)ids;
  (void)result;
  if (error)
    *error = "Vulkan is unavailable in this build";
  return false;
#else
  if (!result || visible.empty() || visible.size() != ids.size() ||
      visible.size() > 65535) {
    if (error)
      *error =
          "cull/compact requires matching non-empty inputs bounded to 65535";
    return false;
  }
  auto &s = *device_.state_;
  const detail::DeviceState::ComputePipelineRecord *p{};
  if (!pipeline(s, "vulkan-f-cull-compact-v1",
                compiler::cull_compact_vulkan_source(), 40, &p, error))
    return false;
  RawBuffer v{}, id{}, c{}, out{};
  const auto destroy = [&] {
    detail::destroy_raw_buffer(s.device_, &v);
    detail::destroy_raw_buffer(s.device_, &id);
    detail::destroy_raw_buffer(s.device_, &c);
    detail::destroy_raw_buffer(s.device_, &out);
  };
  const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!make_buffer(s, visible.size() * 4, usage, true, &v, error) ||
      !make_buffer(s, ids.size() * 4, usage, true, &id, error) ||
      !make_buffer(s, 4, usage, true, &c, error) ||
      !make_buffer(s, ids.size() * 4, usage, true, &out, error)) {
    destroy();
    return false;
  }
  std::memcpy(v.mapped, visible.data(), visible.size() * 4);
  std::memcpy(id.mapped, ids.data(), ids.size() * 4);
  std::memset(c.mapped, 0, 4);
  std::memset(out.mapped, 0, ids.size() * 4);
  VkCommandBuffer cb{};
  if (!begin(s, &cb, error)) {
    destroy();
    return false;
  }
  struct Pc {
    VkDeviceAddress v, id, c, out;
    uint32_t n;
  } pc{v.address, id.address, c.address, out.address,
       static_cast<uint32_t>(ids.size())};
  bind_and_push(cb, p, pc);
  vkCmdDispatch(cb, static_cast<uint32_t>(ids.size()), 1, 1);
  compute_to(cb, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  if (!finish(s, cb, error)) {
    destroy();
    return false;
  }
  result->visible_count = *static_cast<uint32_t *>(c.mapped);
  if (result->visible_count > ids.size()) {
    destroy();
    if (error)
      *error = "GPU cull/compact count exceeded output capacity";
    return false;
  }
  auto *values = static_cast<uint32_t *>(out.mapped);
  result->compact_ids.assign(values, values + result->visible_count);
  result->gpu_dispatch_count = 1;
  result->report.supported = true;
  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add(
      "transient_buffers", hal::LoweringClass::Direct, 4, ids.size() * 12 + 4,
      "requested logical transient buffer bytes; excludes allocation padding");
  result->report.add("cull_compact", hal::LoweringClass::DevicePass, ids.size(),
                     0,
                     "one GPU invocation per input with atomic append; host "
                     "reads only after compute-to-host visibility");
  destroy();
  return true;
#endif
}

bool AdapterHarness::run_gpu_indexed_address_experiment(
    const std::vector<uint32_t> &input,
    GpuIndexedAddressExperimentResult *result, std::string *error) const {
  if (result) {
    *result = {};
    result->report.backend = hal::BackendKind::Vulkan;
  }
#if !defined(VG_HAS_VULKAN)
  (void)input;
  (void)result;
  if (error)
    *error = "Vulkan is unavailable in this build";
  return false;
#else
  if (!result || input.empty() || input.size() > 1024) {
    if (error)
      *error = "indexed-address requires 1..1024 words";
    return false;
  }
  auto &s = *device_.state_;
  ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/vulkan-indexed-address";
  module.instructions.push_back({"load", 1, 0, 4, 0, 1, 1, 0, "input"});
  module.instructions.push_back({"store", 2, 0, 4, 0x5a, 1, 1, 0, "output"});
  module.declared_effects = {{1, 0, 4, ir::Access::Read, 1},
                             {2, 0, 4, ir::Access::Write, 1}};
  const auto package = compiler::build_indexed_compute_package(module);
  if (!package.ok) {
    if (error)
      *error = package.message;
    return false;
  }
  const detail::DeviceState::ComputePipelineRecord *p{};
  if (!pipeline(s, "vulkan-f-indexed-address-v1",
                package.package.vulkan_glsl_source, 16, &p, error))
    return false;
  RawBuffer in{}, out{};
  const auto destroy = [&] {
    detail::destroy_raw_buffer(s.device_, &in);
    detail::destroy_raw_buffer(s.device_, &out);
  };
  if (!make_buffer(s, input.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   true, &in, error) ||
      !make_buffer(s, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &out,
                   error)) {
    destroy();
    return false;
  }
  std::memcpy(in.mapped, input.data(), input.size() * 4);
  std::memset(out.mapped, 0, 4);
  VkCommandBuffer cb{};
  if (!begin(s, &cb, error)) {
    destroy();
    return false;
  }
  struct Pc {
    uint64_t table[2];
  } pc{{in.address, out.address}};
  bind_and_push(cb, p, pc);
  vkCmdDispatch(cb, 1, 1, 1);
  compute_to(cb, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  if (!finish(s, cb, error)) {
    destroy();
    return false;
  }
  if (*static_cast<uint32_t *>(out.mapped) != 0x5a5a5a5au) {
    destroy();
    if (error)
      *error = "indexed compiler GLSL did not device-write its output";
    return false;
  }
  result->referenced_allocation_count = 2;
  result->gpu_dispatch_count = 1;
  result->report.supported = true;
  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add(
      "transient_buffers", hal::LoweringClass::Direct, 2, input.size() * 4 + 4,
      "requested logical transient buffer bytes; excludes allocation padding");
  result->report.add("indexed_address_table", hal::LoweringClass::Direct, 2,
                     2 * sizeof(uint64_t),
                     "compiler::build_indexed_compute_package Vulkan GLSL "
                     "dereferenced a GPU-address table");
  destroy();
  return true;
#endif
}

bool AdapterHarness::run_gpu_tier2_bucket_experiment(
    const std::vector<uint32_t> &nodes, const std::vector<uint32_t> &authorized,
    GpuTier2ExperimentResult *result, std::string *error) const {
  if (result) {
    *result = {};
    result->report.backend = hal::BackendKind::Vulkan;
  }
#if !defined(VG_HAS_VULKAN)
  (void)nodes;
  (void)authorized;
  (void)result;
  if (error)
    *error = "Vulkan is unavailable in this build";
  return false;
#else
  std::vector<uint32_t> sorted_authorized = authorized;
  std::ranges::sort(sorted_authorized);
  if (!result || nodes.empty() || nodes.size() > 65535 ||
      authorized.size() < 2 || authorized.size() > 16 ||
      std::ranges::adjacent_find(sorted_authorized) !=
          sorted_authorized.end()) {
    if (error)
      *error =
          "tier2 requires 1..65535 tasks and 2..16 distinct authorized classes";
    return false;
  }
  for (uint32_t n : nodes)
    if (std::ranges::find(authorized, n) == authorized.end()) {
      if (error)
        *error = "tier2 refused unauthorized node class";
      return false;
    }
  auto &s = *device_.state_;
  const detail::DeviceState::ComputePipelineRecord *bucket{}, *fill{},
      *target{};
  if (!pipeline(s, "vulkan-f-tier2-bucket-v1", kTier2Bucket, 40, &bucket,
                error) ||
      !pipeline(s, "vulkan-f-tier2-fill-v1", kTier2Fill, 24, &fill, error) ||
      !pipeline(s, "vulkan-f-tier2-target-v1", kTier2Target, 8, &target, error))
    return false;
  RawBuffer n{}, a{}, sel{}, counts{}, args{}, done{};
  const auto destroy = [&] {
    for (RawBuffer *b : {&n, &a, &sel, &counts, &args, &done})
      detail::destroy_raw_buffer(s.device_, b);
  };
  const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!make_buffer(s, nodes.size() * 4, usage, true, &n, error) ||
      !make_buffer(s, authorized.size() * 4, usage, true, &a, error) ||
      !make_buffer(s, nodes.size() * 4, usage, true, &sel, error) ||
      !make_buffer(s, authorized.size() * 4, usage, true, &counts, error) ||
      !make_buffer(s, authorized.size() * sizeof(VkDispatchIndirectCommand),
                   usage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true, &args,
                   error) ||
      !make_buffer(s, 4, usage, true, &done, error)) {
    destroy();
    return false;
  }
  std::memcpy(n.mapped, nodes.data(), nodes.size() * 4);
  std::memcpy(a.mapped, authorized.data(), authorized.size() * 4);
  std::memset(sel.mapped, 0, nodes.size() * 4);
  std::memset(counts.mapped, 0, authorized.size() * 4);
  std::memset(args.mapped, 0,
              authorized.size() * sizeof(VkDispatchIndirectCommand));
  std::memset(done.mapped, 0, 4);
  VkCommandBuffer cb{};
  if (!begin(s, &cb, error)) {
    destroy();
    return false;
  }
  struct BucketPc {
    VkDeviceAddress n, a, s, c;
    uint32_t nt, na;
  } bpc{n.address,
        a.address,
        sel.address,
        counts.address,
        static_cast<uint32_t>(nodes.size()),
        static_cast<uint32_t>(authorized.size())};
  bind_and_push(cb, bucket, bpc);
  vkCmdDispatch(cb, static_cast<uint32_t>(nodes.size()), 1, 1);
  compute_to(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_READ_BIT);
  struct FillPc {
    VkDeviceAddress c, args;
    uint32_t n;
  } fpc{counts.address, args.address, static_cast<uint32_t>(authorized.size())};
  bind_and_push(cb, fill, fpc);
  vkCmdDispatch(cb, static_cast<uint32_t>(authorized.size()), 1, 1);
  compute_to(cb, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
  struct DonePc {
    VkDeviceAddress c;
  } dpc{done.address};
  bind_and_push(cb, target, dpc);
  for (size_t i = 0; i < authorized.size(); ++i)
    vkCmdDispatchIndirect(cb, args.buffer,
                          i * sizeof(VkDispatchIndirectCommand));
  compute_to(cb, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  if (!finish(s, cb, error)) {
    destroy();
    return false;
  }
  result->selected_classes.assign(static_cast<uint32_t *>(sel.mapped),
                                  static_cast<uint32_t *>(sel.mapped) +
                                      nodes.size());
  result->bucket_counts.assign(static_cast<uint32_t *>(counts.mapped),
                               static_cast<uint32_t *>(counts.mapped) +
                                   authorized.size());
  if (*static_cast<uint32_t *>(done.mapped) != nodes.size()) {
    destroy();
    if (error)
      *error = "Tier2 indirect bucket dispatches did not execute every "
               "selected task";
    return false;
  }
  result->host_preprocessed_task_count = static_cast<uint32_t>(nodes.size());
  result->gpu_dispatch_count = 2;
  result->indirect_dispatch_count = static_cast<uint32_t>(authorized.size());
  result->report.supported = true;
  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = 3;
  result->report.queue_wait_count = 1;
  result->report.add(
      "transient_buffers", hal::LoweringClass::Direct, 6,
      nodes.size() * 8 + authorized.size() * 20 + 4,
      "requested logical transient buffer bytes; excludes allocation padding");
  result->report.add("tier2_node_select",
                     hal::LoweringClass::EmulatedDevicePass, nodes.size(), 0,
                     "GPU bucketed only preauthorized node classes; no "
                     "device-selected PSO or public capability");
  result->report.add("tier2_bucket_indirect",
                     hal::LoweringClass::EmulatedDevicePass, authorized.size(),
                     0,
                     "GPU filled and every authorized bucket consumed a "
                     "VkDispatchIndirectCommand");
  result->report.add("tier2_host_preprocess", hal::LoweringClass::HostAssisted,
                     nodes.size(), 0,
                     "host validated bounds, distinct authorization and "
                     "membership, then uploaded "
                     "bounded task classes and authorization table; GPU "
                     "selected buckets and wrote indirect commands");
  destroy();
  return true;
#endif
}

} // namespace vg::vulkan
