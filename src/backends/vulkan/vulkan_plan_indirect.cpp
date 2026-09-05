#include "backends/vulkan/vulkan_plan_indirect.h"

#include "backends/vulkan/vulkan_device_internal.h"

#include <algorithm>
#include <cstring>

namespace vg::vulkan::detail {
namespace {
bool equal(core::NodeTable::Ref a, core::NodeTable::Ref b) {
  return a.index == b.index && a.generation == b.generation;
}
} // namespace

struct PlanIndirectCache {
#if defined(VG_HAS_VULKAN)
  VkShaderModule bucket_module{VK_NULL_HANDLE};
  VkShaderModule fill_module{VK_NULL_HANDLE};
  VkPipelineLayout bucket_layout{VK_NULL_HANDLE};
  VkPipelineLayout fill_layout{VK_NULL_HANDLE};
  VkPipeline bucket_pipeline{VK_NULL_HANDLE};
  VkPipeline fill_pipeline{VK_NULL_HANDLE};
#endif
};

PlanIndirectCache *ensure_plan_indirect_cache(DeviceState &state,
                                              std::string *error) {
#if !defined(VG_HAS_VULKAN)
  (void)state;
  if (error)
    *error = "Vulkan plan indirect cache is unavailable in this build";
  return nullptr;
#else
  if (state.plan_indirect_cache_)
    return state.plan_indirect_cache_;
  constexpr const char *bucket_source = R"(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in; layout(buffer_reference,std430,buffer_reference_align=4) buffer U{uint v[];};
layout(push_constant) uniform P{U selected;U authorized;U counts;U matched;uint count;uint bucket_count;} p;
void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.count)return;uint o=i*2;p.matched.v[i]=0;for(uint b=0;b<p.bucket_count;++b){uint a=b*2;if(p.selected.v[o]==p.authorized.v[a]&&p.selected.v[o+1]==p.authorized.v[a+1]){atomicAdd(p.counts.v[b],1);p.matched.v[i]=b+1;return;}}})";
  constexpr const char *fill_source = R"(#version 450
#extension GL_EXT_buffer_reference2 : require
layout(local_size_x=1) in; layout(buffer_reference,std430,buffer_reference_align=4) buffer U{uint v[];};
layout(push_constant) uniform P{U records;U matched;U commands;uint count;uint indexed;} p;
void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.count)return;uint r=i*6;uint o=i*(p.indexed!=0?5:4);if(p.matched.v[i]==0){for(uint w=0;w<(p.indexed!=0?5:4);++w)p.commands.v[o+w]=0;return;}p.commands.v[o]=p.records.v[r];p.commands.v[o+1]=p.records.v[r+1];p.commands.v[o+2]=p.records.v[r+2];p.commands.v[o+3]=(p.indexed!=0?p.records.v[r+3]:p.records.v[r+4]);if(p.indexed!=0)p.commands.v[o+4]=p.records.v[r+4];})";
  auto *cache = new PlanIndirectCache();
  const auto clean = [&] {
    destroy_plan_indirect_cache(state);
    delete cache;
  };
  std::vector<uint32_t> spirv;
  if (!compile_glsl_stage(bucket_source, "compute", {}, &spirv, error)) {
    delete cache;
    return nullptr;
  }
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.codeSize = spirv.size() * 4;
  sm.pCode = spirv.data();
  if (vkCreateShaderModule(state.device_, &sm, nullptr,
                           &cache->bucket_module) != VK_SUCCESS) {
    if (error)
      *error = "vkCreateShaderModule failed for Tier2 bucket";
    delete cache;
    return nullptr;
  }
  if (!compile_glsl_stage(fill_source, "compute", {}, &spirv, error)) {
    vkDestroyShaderModule(state.device_, cache->bucket_module, nullptr);
    delete cache;
    return nullptr;
  }
  sm.codeSize = spirv.size() * 4;
  sm.pCode = spirv.data();
  if (vkCreateShaderModule(state.device_, &sm, nullptr, &cache->fill_module) !=
      VK_SUCCESS) {
    if (error)
      *error = "vkCreateShaderModule failed for Tier2 fill";
    vkDestroyShaderModule(state.device_, cache->bucket_module, nullptr);
    delete cache;
    return nullptr;
  }
  auto layout = [&](VkPipelineLayout *out, uint32_t push_size) {
    VkPushConstantRange r{};
    r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    r.size = push_size;
    VkPipelineLayoutCreateInfo i{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    i.pushConstantRangeCount = 1;
    i.pPushConstantRanges = &r;
    return vkCreatePipelineLayout(state.device_, &i, nullptr, out) ==
           VK_SUCCESS;
  };
  if (!layout(&cache->bucket_layout, 40) || !layout(&cache->fill_layout, 32)) {
    if (error)
      *error = "vkCreatePipelineLayout failed for Tier2 producer";
    if (cache->bucket_layout)
      vkDestroyPipelineLayout(state.device_, cache->bucket_layout, nullptr);
    if (cache->fill_layout)
      vkDestroyPipelineLayout(state.device_, cache->fill_layout, nullptr);
    vkDestroyShaderModule(state.device_, cache->bucket_module, nullptr);
    vkDestroyShaderModule(state.device_, cache->fill_module, nullptr);
    delete cache;
    return nullptr;
  }
  auto pipe = [&](VkShaderModule module, VkPipelineLayout layout,
                  VkPipeline *out) {
    VkPipelineShaderStageCreateInfo s{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    s.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    s.module = module;
    s.pName = "main";
    VkComputePipelineCreateInfo i{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    i.stage = s;
    i.layout = layout;
    return vkCreateComputePipelines(state.device_, VK_NULL_HANDLE, 1, &i,
                                    nullptr, out) == VK_SUCCESS;
  };
  if (!pipe(cache->bucket_module, cache->bucket_layout,
            &cache->bucket_pipeline) ||
      !pipe(cache->fill_module, cache->fill_layout, &cache->fill_pipeline)) {
    if (error)
      *error = "vkCreateComputePipelines failed for Tier2 producer";
    if (cache->bucket_pipeline)
      vkDestroyPipeline(state.device_, cache->bucket_pipeline, nullptr);
    if (cache->fill_pipeline)
      vkDestroyPipeline(state.device_, cache->fill_pipeline, nullptr);
    vkDestroyPipelineLayout(state.device_, cache->bucket_layout, nullptr);
    vkDestroyPipelineLayout(state.device_, cache->fill_layout, nullptr);
    vkDestroyShaderModule(state.device_, cache->bucket_module, nullptr);
    vkDestroyShaderModule(state.device_, cache->fill_module, nullptr);
    delete cache;
    return nullptr;
  }
  state.plan_indirect_cache_ = cache;
  return cache;
#endif
}

void destroy_plan_indirect_cache(DeviceState &state) {
#if defined(VG_HAS_VULKAN)
  auto *c = state.plan_indirect_cache_;
  if (!c)
    return;
  if (c->bucket_pipeline)
    vkDestroyPipeline(state.device_, c->bucket_pipeline, nullptr);
  if (c->fill_pipeline)
    vkDestroyPipeline(state.device_, c->fill_pipeline, nullptr);
  if (c->bucket_layout)
    vkDestroyPipelineLayout(state.device_, c->bucket_layout, nullptr);
  if (c->fill_layout)
    vkDestroyPipelineLayout(state.device_, c->fill_layout, nullptr);
  if (c->bucket_module)
    vkDestroyShaderModule(state.device_, c->bucket_module, nullptr);
  if (c->fill_module)
    vkDestroyShaderModule(state.device_, c->fill_module, nullptr);
  delete c;
  state.plan_indirect_cache_ = nullptr;
#else
  (void)state;
#endif
}

bool generate_plan_tier2_draw_commands(
    DeviceState &state, const std::vector<tier2::SelectionRecord> &records,
    const std::vector<tier2::AuthorizedBucket> &authorized, bool indexed_draw,
    GpuDrawCommandView *view, PlanIndirectStats *stats,
    hal::LoweringReport *report, std::string *error) {
  if (view)
    *view = {};
  if (stats)
    *stats = {};
  tier2::ValidatedSelection selection;
  if (!view || !stats || !report ||
      !tier2::validate_pre_authorized_selection(records, authorized, &selection,
                                                error))
    return false;
#if !defined(VG_HAS_VULKAN)
  (void)state;
  (void)indexed_draw;
  if (error)
    *error = "Vulkan Tier2 GPU command generation is unavailable in this build";
  return false;
#else
  auto *cache = ensure_plan_indirect_cache(state, error);
  if (cache == nullptr)
    return false;
  const uint32_t task_count = static_cast<uint32_t>(records.size());
  const uint32_t bucket_count = static_cast<uint32_t>(authorized.size());
  const uint32_t stride = indexed_draw ? 20u : 16u;
  for (const auto &record : records) {
    if (record.indexed_draw != indexed_draw) {
      if (error)
        *error = "Tier2 selected Raster tasks must use one indirect command ABI";
      return false;
    }
  }
  RawBuffer selected{}, authorized_nodes{}, counts{}, matched{}, draw_inputs{}, commands{};
  const auto destroy_temporary = [&] {
    destroy_raw_buffer(state.device_, &selected);
    destroy_raw_buffer(state.device_, &authorized_nodes);
    destroy_raw_buffer(state.device_, &counts);
    destroy_raw_buffer(state.device_, &matched);
    destroy_raw_buffer(state.device_, &draw_inputs);
    destroy_raw_buffer(state.device_, &commands);
  };
  if (!create_raw_buffer(
          state.device_, state.physical_device_, task_count * 2 * sizeof(uint32_t),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true, &selected, error) ||
      !create_raw_buffer(state.device_, state.physical_device_,
                         bucket_count * 2 * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true,
                         &authorized_nodes, error) ||
      !create_raw_buffer(state.device_, state.physical_device_,
                         bucket_count * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true,
                         &counts, error) ||
      !create_raw_buffer(state.device_, state.physical_device_,
                         task_count * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false,
                         &matched, error) ||
      !create_raw_buffer(state.device_, state.physical_device_,
                         task_count * 6 * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true,
                         &draw_inputs, error) ||
      !create_raw_buffer(state.device_, state.physical_device_,
                         static_cast<VkDeviceSize>(task_count) * stride,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                         true, false, &commands, error)) {
    destroy_temporary();
    return false;
  }
  auto *selected_words = static_cast<uint32_t *>(selected.mapped);
  auto *authorized_words = static_cast<uint32_t *>(authorized_nodes.mapped);
  for (uint32_t i = 0; i < task_count; ++i) {
    selected_words[i * 2] = records[i].node.index;
    selected_words[i * 2 + 1] = records[i].node.generation;
  }
  for (uint32_t i = 0; i < bucket_count; ++i) {
    authorized_words[i * 2] = authorized[i].node.index;
    authorized_words[i * 2 + 1] = authorized[i].node.generation;
  }
  std::memset(counts.mapped, 0, bucket_count * sizeof(uint32_t));
  auto *draw_words = static_cast<uint32_t *>(draw_inputs.mapped);
  for (uint32_t i = 0; i < task_count; ++i) {
    const auto &record = records[i];
    const uint32_t word = i * 6;
    draw_words[word] = record.vertex_or_index_count;
    draw_words[word + 1] = record.instance_count;
    draw_words[word + 2] = record.first_vertex_or_index;
    draw_words[word + 3] = static_cast<uint32_t>(record.vertex_offset);
    draw_words[word + 4] = record.first_instance;
    draw_words[word + 5] = record.indexed_draw ? 1u : 0u;
  }
  if (!ensure_command_pool(state.device_, state.compute_queue_family_,
                           &state.command_pool_, error)) {
    destroy_temporary();
    return false;
  }
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (!allocate_command_buffer(state.device_, state.command_pool_, &cb,
                               error)) {
    destroy_temporary();
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
    vkFreeCommandBuffers(state.device_, state.command_pool_, 1, &cb);
    destroy_temporary();
    if (error)
      *error = "vkBeginCommandBuffer failed for Tier2 draw producer";
    return false;
  }
  struct BucketPush {
    VkDeviceAddress selected, authorized, counts, matched;
    uint32_t count, bucket_count;
  } bucket_push{selected.address, authorized_nodes.address, counts.address,
                matched.address, task_count, bucket_count};
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cache->bucket_pipeline);
  vkCmdPushConstants(cb, cache->bucket_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(bucket_push), &bucket_push);
  vkCmdDispatch(cb, task_count, 1, 1);
  VkMemoryBarrier2 bucket_to_fill{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  bucket_to_fill.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  bucket_to_fill.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  bucket_to_fill.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  bucket_to_fill.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  VkDependencyInfo bucket_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  bucket_dependency.memoryBarrierCount = 1;
  bucket_dependency.pMemoryBarriers = &bucket_to_fill;
  vkCmdPipelineBarrier2(cb, &bucket_dependency);
  struct FillPush {
    VkDeviceAddress records, matched, commands;
    uint32_t count, indexed;
  } fill_push{draw_inputs.address, matched.address, commands.address, task_count,
              indexed_draw ? 1u : 0u};
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cache->fill_pipeline);
  vkCmdPushConstants(cb, cache->fill_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(fill_push), &fill_push);
  vkCmdDispatch(cb, task_count, 1, 1);
  VkMemoryBarrier2 fill_to_draw{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  fill_to_draw.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  fill_to_draw.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  fill_to_draw.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                              VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
  fill_to_draw.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                               VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
  VkDependencyInfo draw_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  draw_dependency.memoryBarrierCount = 1;
  draw_dependency.pMemoryBarriers = &fill_to_draw;
  vkCmdPipelineBarrier2(cb, &draw_dependency);
  if (vkEndCommandBuffer(cb) != VK_SUCCESS ||
      !submit_and_wait_simple(state.device_, state.compute_queue_,
                              state.command_pool_, cb, error)) {
    destroy_temporary();
    return false;
  }
  destroy_raw_buffer(state.device_, &selected);
  destroy_raw_buffer(state.device_, &authorized_nodes);
  destroy_raw_buffer(state.device_, &counts);
  destroy_raw_buffer(state.device_, &matched);
  destroy_raw_buffer(state.device_, &draw_inputs);
  view->buffer = commands.buffer;
  view->memory = commands.memory;
  view->command_count = task_count;
  view->command_stride = stride;
  view->byte_size = static_cast<uint64_t>(task_count) * stride;
  commands = {};
  stats->command_buffer_count = 1;
  stats->encoder_count = 2;
  stats->barrier_count = 2;
  stats->queue_wait_count = 1;
  stats->indirect_draw_count = task_count;
  stats->temporary_bytes = static_cast<uint64_t>(task_count) * 8 +
                           static_cast<uint64_t>(bucket_count) * 12 +
                           static_cast<uint64_t>(task_count) * (4 + 6 * 4 + stride);
  report->backend = hal::BackendKind::Vulkan;
  report->supported = true;
  report->command_buffer_count = 1;
  report->encoder_count = 2;
  report->barrier_count = 2;
  report->queue_wait_count = 1;
  report->add("tier2_bucket_fill_draw_commands",
              hal::LoweringClass::EmulatedDevicePass, task_count,
              stats->temporary_bytes,
              "GPU full-NodeRef authorization/count pass writes per-task matches; "
              "the fill pass consumes those matches to author indirect commands");
  return true;
#endif
}

void destroy_plan_tier2_draw_commands(DeviceState &state,
                                      GpuDrawCommandView *view) {
  if (!view)
    return;
#if defined(VG_HAS_VULKAN)
  if (view->buffer != VK_NULL_HANDLE)
    vkDestroyBuffer(state.device_, view->buffer, nullptr);
  if (view->memory != VK_NULL_HANDLE)
    vkFreeMemory(state.device_, view->memory, nullptr);
#else
  (void)state;
#endif
  *view = {};
}

#if defined(VG_HAS_VULKAN)
bool record_plan_tier2_draw_consumer(
    VkCommandBuffer command_buffer, const GpuDrawCommandView &view,
    bool indexed_draw, const PlanIndirectBucketBinder &bind_bucket,
    uint32_t first_command, uint32_t command_count, PlanIndirectStats *stats,
    std::string *error) {
  const uint32_t expected_stride = indexed_draw ? 20u : 16u;
  if (command_buffer == VK_NULL_HANDLE || view.buffer == VK_NULL_HANDLE ||
      command_count == 0 || first_command >= view.command_count ||
      command_count > view.command_count - first_command ||
      view.command_stride != expected_stride ||
      view.byte_size <
          static_cast<uint64_t>(view.command_count) * expected_stride ||
      !bind_bucket) {
    if (error)
      *error =
          "Tier2 draw consumer received an invalid GPU indirect command view";
    return false;
  }
  for (uint32_t command_index = first_command;
       command_index < first_command + command_count; ++command_index) {
    if (!bind_bucket(command_buffer, command_index, error))
      return false;
    const VkDeviceSize offset =
        static_cast<VkDeviceSize>(command_index) * view.command_stride;
    if (indexed_draw)
      vkCmdDrawIndexedIndirect(command_buffer, view.buffer, offset, 1,
                               view.command_stride);
    else
      vkCmdDrawIndirect(command_buffer, view.buffer, offset, 1,
                        view.command_stride);
  }
  if (stats)
    stats->indirect_draw_count += command_count;
  return true;
}
#endif

bool submit_plan_tier2_indirect(
    DeviceState &state, const core::ExecutionPlan &plan,
    const std::vector<hal::CompiledPlan::PerNodePackage> &packages,
    const std::vector<tier2::SelectionRecord> &gpu_selection_records,
    bool indexed_draw, GpuDrawCommandView *view, PlanIndirectStats *stats,
    hal::LoweringReport *report, std::string *error) {
  if (stats)
    *stats = {};
  if (report)
    *report = {};
  if (!plan.tier2_selection_requested ||
      plan.tier2_selection_nodes.size() < 2) {
    if (error)
      *error = "Tier2 indirect submit requires the explicit sealed Core "
               "selection fact";
    return false;
  }
  std::vector<tier2::AuthorizedBucket> authorized;
  authorized.reserve(plan.tier2_selection_nodes.size());
  for (uint32_t slot = 0; slot < plan.tier2_selection_nodes.size(); ++slot) {
    const auto ref = plan.tier2_selection_nodes[slot];
    const auto package =
        std::ranges::find_if(packages, [ref](const auto &candidate) {
          return equal(candidate.ref, ref);
        });
    if (package == packages.end() ||
        package->kind != hal::CompiledPlan::NodePackageKind::Raster) {
      if (error)
        *error = "Tier2 indirect submit is missing a complete sealed Raster "
                 "package for an authorized NodeRef";
      return false;
    }
    authorized.push_back({ref, slot, slot});
  }
  // Stage7 owns the rendering scope: it consumes this view through
  // record_plan_tier2_draw_consumer() and destroys it afterwards. The producer
  // never maps or reads back the GPU-written counts or draw commands.
  return generate_plan_tier2_draw_commands(state, gpu_selection_records,
                                           authorized, indexed_draw, view,
                                           stats, report, error);
}
} // namespace vg::vulkan::detail
