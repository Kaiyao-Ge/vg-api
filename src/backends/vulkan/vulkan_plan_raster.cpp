#include "backends/vulkan/vulkan_plan_raster.h"

#include "backends/vulkan/vulkan_device_internal.h"
#include "backends/vulkan/vulkan_user_raster.h"
#include "core/scene_root.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>

namespace vg::vulkan::detail {
namespace {
struct RasterVertexAbi {
  float x, y, z, u, v;
};
static_assert(sizeof(RasterVertexAbi) == 5 * sizeof(float));

// Deliberately separate from compiler::raster_facet_vulkan_source(): that
// source belongs to D's four-float direct probe and its three-binding layout.
// E1 compiles this formal ABI into its own cache once DeviceState owns the
// matching descriptor/pipeline resources.
std::string formal_raster_glsl() {
  return "#version 450\n"
         "#ifdef VG_RASTER_VERTEX_STAGE\n"
         "struct V { float x; float y; float z; float u; float v; };\n"
         "layout(set=0,binding=0,std430) readonly buffer VB { V v[]; };\n"
         "layout(set=0,binding=3,std140) uniform Root { mat4 camera; vec4 "
         "color; uvec2 albedo; } root;\n"
         "layout(location=0) out vec2 uv;\n"
         "void main(){ V a=v[gl_VertexIndex]; "
         "gl_Position=root.camera*vec4(a.x,a.y,a.z,1); uv=vec2(a.u,a.v); }\n"
         "#endif\n#ifdef VG_RASTER_FRAGMENT_STAGE\n"
         "layout(set=0,binding=1) uniform sampler2D tex;\n"
         "layout(set=0,binding=2,std140) uniform Tint { vec4 tint; };\n"
         "layout(location=0) in vec2 uv; layout(location=0) out vec4 "
         "out_color;\n"
         "void main(){ out_color=texture(tex,uv)*tint; }\n#endif\n";
}
bool equal(core::NodeTable::Ref a, core::NodeTable::Ref b) {
  return a.index == b.index && a.generation == b.generation;
}

const core::ExecutionPlan::ResolvedNode *
find_node(const core::ExecutionPlan &plan, core::NodeTable::Ref ref) {
  const auto it =
      std::ranges::find_if(plan.resolved_nodes, [ref](const auto &node) {
        return equal(node.ref, ref);
      });
  return it == plan.resolved_nodes.end() ? nullptr : &*it;
}

const hal::CompiledPlan::PerNodePackage *
find_package(const hal::CompiledPlan &compiled, core::NodeTable::Ref ref) {
  const auto it = std::ranges::find_if(
      compiled.per_node_packages,
      [ref](const auto &package) { return equal(package.ref, ref); });
  return it == compiled.per_node_packages.end() ? nullptr : &*it;
}

bool validate_address_facet(const core::Arena &arena, core::FacetPool &pool,
                            core::FacetRef ref, const char *label,
                            const core::FacetSlot **out, std::string *error) {
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot *slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr || slot->kind != core::FacetKind::Address) {
    if (error)
      *error =
          std::string(label) + ": " +
          (slot == nullptr ? core::to_string(status) : "facet kind mismatch");
    return false;
  }
  *out = slot;
  return true;
}

bool validate_raster_task(const hal::CompiledPlan &compiled,
                          uint32_t task_index, core::Arena &arena,
                          core::FacetPool &pool, std::string *error) {
  const auto &tasks = compiled.plan.task_graph.tasks();
  if (task_index >= tasks.size()) {
    if (error)
      *error = "sealed schedule names an out-of-range Raster task";
    return false;
  }
  const core::TaskRecord &task = tasks[task_index];
  if (task.kind != core::TaskKind::Raster) {
    if (error)
      *error = "Raster step received a non-Raster task";
    return false;
  }
  const core::NodeTable::Ref ref{task.node_index, task.node_generation};
  const auto *node = find_node(compiled.plan, ref);
  const auto *package = find_package(compiled, ref);
  if (node == nullptr || package == nullptr ||
      package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
      package->package.has_value()) {
    if (error)
      *error = "Raster task resolved a non-raster immutable NodeRef package";
    return false;
  }
  if (node->user_raster_shader.has_value()) {
    if (!node->code_object ||
        node->code_object->format_tag != "vg.glsl.raster/v1") {
      if (error)
        *error = "Vulkan user Raster requires code object format "
                 "vg.glsl.raster/v1; MSL is Unsupported";
      return false;
    }
  } else if (!node->module.has_value()) {
    if (error)
      *error = "built-in Raster NodeRef has no immutable module";
    return false;
  }
  const core::FacetSlot *vertex_slot = nullptr;
  if (!validate_address_facet(arena, pool, task.vertex_buffer_ref,
                              "raster vertex buffer", &vertex_slot, error))
    return false;
  const core::Allocation *vertices = arena.lookup(core::PointerRef{
      vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
  if (vertices == nullptr ||
      vertices->bytes.size() % sizeof(RasterVertexAbi) != 0) {
    if (error)
      *error = "raster task vertex buffer byte size is not a multiple of "
               "sizeof(RasterVertex)";
    return false;
  }
  for (size_t offset = 0; offset < vertices->bytes.size();
       offset += sizeof(RasterVertexAbi)) {
    RasterVertexAbi vertex{};
    std::memcpy(&vertex, vertices->bytes.data() + offset, sizeof(vertex));
    if (!std::isfinite(vertex.z) || vertex.z < 0.0f || vertex.z > 1.0f) {
      if (error)
        *error = "raster task vertex z must be finite and normalized to [0,1] "
                 "(F4 vertex ABI)";
      return false;
    }
  }
  if (task.index_count != 0) {
    const core::FacetSlot *index_slot = nullptr;
    if (!validate_address_facet(arena, pool, task.index_buffer_ref,
                                "raster index buffer", &index_slot, error))
      return false;
    const size_t stride =
        index_slot->view.format == core::PixelFormat::R16Uint ? sizeof(uint16_t)
        : index_slot->view.format == core::PixelFormat::R32Uint
            ? sizeof(uint32_t)
            : 0;
    if (stride == 0 || task.index_count % 3 != 0 ||
        task.index_count > std::numeric_limits<size_t>::max() / stride) {
      if (error)
        *error = "raster task index buffer requires R16Uint/R32Uint and a "
                 "triangle-list count";
      return false;
    }
    const core::Allocation *indices = arena.lookup(core::PointerRef{
        index_slot->view.allocation, index_slot->view.allocation_generation});
    if (indices == nullptr ||
        indices->bytes.size() < task.index_count * stride) {
      if (error)
        *error = "raster task index buffer is shorter than index_count";
      return false;
    }
  }
  return true;
}

#if defined(VG_HAS_VULKAN)
VkCompareOp to_vk_depth_compare(core::DepthCompareOp op) {
  switch (op) {
  case core::DepthCompareOp::Never:
    return VK_COMPARE_OP_NEVER;
  case core::DepthCompareOp::Less:
    return VK_COMPARE_OP_LESS;
  case core::DepthCompareOp::Equal:
    return VK_COMPARE_OP_EQUAL;
  case core::DepthCompareOp::LessEqual:
    return VK_COMPARE_OP_LESS_OR_EQUAL;
  case core::DepthCompareOp::Greater:
    return VK_COMPARE_OP_GREATER;
  case core::DepthCompareOp::NotEqual:
    return VK_COMPARE_OP_NOT_EQUAL;
  case core::DepthCompareOp::GreaterEqual:
    return VK_COMPARE_OP_GREATER_OR_EQUAL;
  case core::DepthCompareOp::Always:
    return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_ALWAYS;
}
#endif
} // namespace

struct PlanRasterCache {
  // SPIR-V has no VkDevice ownership. Its immutable key is retained here while
  // the device-specific modules below are destroyed before DeviceState dies.
  UserRasterSpirvCache user_spirv_cache;
#if defined(VG_HAS_VULKAN)
  VkDescriptorSetLayout set_layout{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  VkShaderModule vertex_module{VK_NULL_HANDLE};
  VkShaderModule fragment_module{VK_NULL_HANDLE};
  std::map<std::string, UserRasterShaderModules> user_modules;
  std::map<std::string, VkPipeline> pipelines;
#endif
};

#if defined(VG_HAS_VULKAN)
struct PlanRasterBindings {
  FacetUseGuard target_use;
  FacetUseGuard source_use;
  std::optional<FacetUseGuard> depth_use;
  RawBuffer vertices{};
  RawBuffer tint{};
  RawBuffer root{};
  RawBuffer index{};
  VkIndexType index_type{VK_INDEX_TYPE_UINT16};
  DeviceState::VulkanFacetRecord *target{};
  DeviceState::VulkanFacetRecord *source{};
  DeviceState::VulkanFacetRecord *depth{};
  VkDescriptorSet set{VK_NULL_HANDLE};
  PlanRasterBindings(core::FacetPool &pool, core::FacetRef target_ref,
                     core::FacetRef source_ref)
      : target_use(pool, target_ref), source_use(pool, source_ref) {}
  ~PlanRasterBindings() = default;
};

void destroy_plan_raster_bindings(DeviceState &state,
                                  PlanRasterBindings *value) {
  destroy_raw_buffer(state.device_, &value->vertices);
  destroy_raw_buffer(state.device_, &value->tint);
  destroy_raw_buffer(state.device_, &value->root);
  destroy_raw_buffer(state.device_, &value->index);
}

bool prepare_plan_raster_bindings(DeviceState &state,
                                  const core::TaskRecord &task,
                                  core::FacetRef source_ref,
                                  const std::array<float, 4> &tint_value,
                                  const std::array<uint8_t, 80> &root_bytes,
                                  const core::Allocation &vertex_bytes,
                                  core::Arena &arena, PlanRasterCache &cache,
                                  PlanRasterBindings *out, std::string *error) {
  if (!out->target_use.begin(arena, error) ||
      !out->source_use.begin(arena, error))
    return false;
  const bool has_depth = task.depth_attachment_ref.index != 0 ||
                         task.depth_attachment_ref.generation != 0;
  if (has_depth) {
    out->depth_use.emplace(state.facet_pool(), task.depth_attachment_ref);
    if (!out->depth_use->begin(arena, error))
      return false;
  }
  bool hit = false;
  uint64_t temporary = 0;
  if (!state.ensure_facet_image(arena, state.facet_pool(),
                                task.raster_facets.target,
                                core::FacetKind::Attachment, VK_NULL_HANDLE, 0,
                                &out->target, &hit, &temporary, error) ||
      !state.ensure_facet_image(arena, state.facet_pool(), source_ref,
                                core::FacetKind::Sample, VK_NULL_HANDLE, 0,
                                &out->source, &hit, &temporary, error) ||
      (has_depth && !state.ensure_facet_image(
                        arena, state.facet_pool(), task.depth_attachment_ref,
                        core::FacetKind::Attachment, VK_NULL_HANDLE, 0,
                        &out->depth, &hit, &temporary, error)))
    return false;
  if (!create_raw_buffer(state.device_, state.physical_device_,
                         vertex_bytes.bytes.size(),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         false, true, &out->vertices, error) ||
      !create_raw_buffer(state.device_, state.physical_device_, 16,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false, true,
                         &out->tint, error) ||
      !create_raw_buffer(state.device_, state.physical_device_, 80,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false, true,
                         &out->root, error)) {
    destroy_plan_raster_bindings(state, out);
    return false;
  }
  std::memcpy(out->vertices.mapped, vertex_bytes.bytes.data(),
              vertex_bytes.bytes.size());
  if (task.index_count != 0) {
    const auto *slot = DeviceState::resolve_facet(
        arena, state.facet_pool(), task.index_buffer_ref,
        core::FacetKind::Address, error);
    const auto *bytes =
        slot == nullptr
            ? nullptr
            : arena.lookup(core::PointerRef{slot->view.allocation,
                                            slot->view.allocation_generation});
    const size_t stride =
        slot != nullptr && slot->view.format == core::PixelFormat::R16Uint ? 2
        : slot != nullptr && slot->view.format == core::PixelFormat::R32Uint
            ? 4
            : 0;
    if (bytes == nullptr || stride == 0 ||
        task.index_count > bytes->bytes.size() / stride) {
      if (error)
        *error = "Raster index facet requires R16Uint/R32Uint bytes covering "
                 "index_count";
      destroy_plan_raster_bindings(state, out);
      return false;
    }
    out->index_type = stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (!create_raw_buffer(state.device_, state.physical_device_,
                           bytes->bytes.size(),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false, true,
                           &out->index, error)) {
      destroy_plan_raster_bindings(state, out);
      return false;
    }
    std::memcpy(out->index.mapped, bytes->bytes.data(), bytes->bytes.size());
  }
  std::memcpy(out->tint.mapped, tint_value.data(), 16);
  std::memcpy(out->root.mapped, root_bytes.data(), root_bytes.size());
  if (!state.ensure_descriptor_pool(error)) {
    destroy_plan_raster_bindings(state, out);
    return false;
  }
  VkDescriptorSetAllocateInfo alloc{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = state.descriptor_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &cache.set_layout;
  if (vkAllocateDescriptorSets(state.device_, &alloc, &out->set) !=
      VK_SUCCESS) {
    if (error)
      *error = "formal Raster descriptor allocation failed";
    destroy_plan_raster_bindings(state, out);
    return false;
  }
  VkDescriptorBufferInfo vb{out->vertices.buffer, 0, VK_WHOLE_SIZE},
      tint{out->tint.buffer, 0, VK_WHOLE_SIZE},
      root{out->root.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo image{};
  VkSampler sampler = VK_NULL_HANDLE;
  if (!state.ensure_sampler(task.raster_filter, task.raster_wrap, &sampler,
                            error)) {
    destroy_plan_raster_bindings(state, out);
    return false;
  }
  image.sampler = sampler;
  image.imageView = out->source->view;
  image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet writes[4]{};
  for (auto &w : writes)
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = out->set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &vb;
  writes[1].dstSet = out->set;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].pImageInfo = &image;
  writes[2].dstSet = out->set;
  writes[2].dstBinding = 2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[2].pBufferInfo = &tint;
  writes[3].dstSet = out->set;
  writes[3].dstBinding = 3;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[3].pBufferInfo = &root;
  vkUpdateDescriptorSets(state.device_, 4, writes, 0, nullptr);
  return true;
}
#endif

PlanRasterCache *ensure_plan_raster_cache(DeviceState &state,
                                          std::string *error) {
#if !defined(VG_HAS_VULKAN)
  (void)state;
  if (error)
    *error = "Vulkan adapter is unavailable in this build";
  return nullptr;
#else
  if (state.plan_raster_cache_ != nullptr)
    return state.plan_raster_cache_;
  auto *cache = new PlanRasterCache();
  VkDescriptorSetLayoutBinding bindings[4] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
       nullptr}};
  VkDescriptorSetLayoutCreateInfo set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(state.device_, &set_info, nullptr,
                                  &cache->set_layout) != VK_SUCCESS) {
    delete cache;
    if (error)
      *error = "vkCreateDescriptorSetLayout failed for formal Raster";
    return nullptr;
  }
  VkPipelineLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &cache->set_layout;
  if (vkCreatePipelineLayout(state.device_, &layout_info, nullptr,
                             &cache->pipeline_layout) != VK_SUCCESS) {
    vkDestroyDescriptorSetLayout(state.device_, cache->set_layout, nullptr);
    delete cache;
    if (error)
      *error = "vkCreatePipelineLayout failed for formal Raster";
    return nullptr;
  }
  std::vector<uint32_t> spirv;
  if (!compile_glsl_stage(formal_raster_glsl(), "vertex",
                          {"VG_RASTER_VERTEX_STAGE"}, &spirv, error)) {
    vkDestroyPipelineLayout(state.device_, cache->pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(state.device_, cache->set_layout, nullptr);
    delete cache;
    return nullptr;
  }
  VkShaderModuleCreateInfo module_info{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(state.device_, &module_info, nullptr,
                           &cache->vertex_module) != VK_SUCCESS ||
      !compile_glsl_stage(formal_raster_glsl(), "fragment",
                          {"VG_RASTER_FRAGMENT_STAGE"}, &spirv, error)) {
    if (cache->vertex_module != VK_NULL_HANDLE)
      vkDestroyShaderModule(state.device_, cache->vertex_module, nullptr);
    vkDestroyPipelineLayout(state.device_, cache->pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(state.device_, cache->set_layout, nullptr);
    delete cache;
    return nullptr;
  }
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(state.device_, &module_info, nullptr,
                           &cache->fragment_module) != VK_SUCCESS) {
    vkDestroyShaderModule(state.device_, cache->vertex_module, nullptr);
    vkDestroyPipelineLayout(state.device_, cache->pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(state.device_, cache->set_layout, nullptr);
    delete cache;
    if (error)
      *error = "vkCreateShaderModule failed for formal Raster fragment";
    return nullptr;
  }
  state.plan_raster_cache_ = cache;
  return cache;
#endif
}

void destroy_plan_raster_cache(DeviceState &state) {
#if !defined(VG_HAS_VULKAN)
  (void)state;
#else
  auto *cache = state.plan_raster_cache_;
  if (cache == nullptr)
    return;
  for (const auto &[key, pipeline] : cache->pipelines) {
    (void)key;
    vkDestroyPipeline(state.device_, pipeline, nullptr);
  }
  for (auto &[key, modules] : cache->user_modules) {
    (void)key;
    destroy_user_raster_shader_modules(state.device_, &modules);
  }
  if (cache->fragment_module != VK_NULL_HANDLE)
    vkDestroyShaderModule(state.device_, cache->fragment_module, nullptr);
  if (cache->vertex_module != VK_NULL_HANDLE)
    vkDestroyShaderModule(state.device_, cache->vertex_module, nullptr);
  if (cache->pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(state.device_, cache->pipeline_layout, nullptr);
  if (cache->set_layout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(state.device_, cache->set_layout, nullptr);
  delete cache;
  state.plan_raster_cache_ = nullptr;
#endif
}

#if defined(VG_HAS_VULKAN)
bool ensure_plan_raster_pipeline_impl(
    DeviceState &state, VkFormat color_format, VkFormat depth_format,
    bool has_depth, uint32_t sample_count, bool depth_test_enable,
    bool depth_write_enable, core::DepthCompareOp depth_compare_op,
    const UserRasterShaderModules *user_modules, const std::string &program_key,
    VkPipeline *out, bool *cache_hit, std::string *error) {
  if (out == nullptr || cache_hit == nullptr)
    return false;
  if (has_depth && depth_format == VK_FORMAT_UNDEFINED) {
    if (error)
      *error = "formal Raster depth pipeline requires a depth format";
    return false;
  }
  auto *cache = ensure_plan_raster_cache(state, error);
  if (cache == nullptr)
    return false;
  const std::string key =
      program_key + ":color=" + std::to_string(static_cast<int>(color_format)) +
      ":depth=" + std::to_string(static_cast<int>(depth_format)) +
      ":has-depth=" + (has_depth ? "1" : "0") +
      ":samples=" + std::to_string(sample_count) +
      ":depth-test=" + (depth_test_enable ? "1" : "0") +
      ":depth-write=" + (depth_write_enable ? "1" : "0") +
      ":depth-compare=" + std::to_string(static_cast<int>(depth_compare_op));
  if (const auto it = cache->pipelines.find(key);
      it != cache->pipelines.end()) {
    *out = it->second;
    *cache_hit = true;
    return true;
  }
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module =
      user_modules == nullptr ? cache->vertex_module : user_modules->vertex;
  stages[0].pName = "main";
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module =
      user_modules == nullptr ? cache->fragment_module : user_modules->fragment;
  stages[1].pName = "main";
  VkVertexInputBindingDescription binding{0, sizeof(RasterVertexAbi),
                                          VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[2]{
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
  VkPipelineVertexInputStateCreateInfo vertex{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex.vertexBindingDescriptionCount = 1;
  vertex.pVertexBindingDescriptions = &binding;
  vertex.vertexAttributeDescriptionCount = 2;
  vertex.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = to_vk_sample_count(sample_count);
  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color.attachmentCount = 1;
  color.pAttachments = &blend;
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = has_depth && depth_test_enable ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = has_depth && depth_write_enable ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = to_vk_depth_compare(depth_compare_op);
  depth.depthBoundsTestEnable = VK_FALSE;
  depth.stencilTestEnable = VK_FALSE;
  VkDynamicState dynamic_states[2]{VK_DYNAMIC_STATE_VIEWPORT,
                                   VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;
  rendering.depthAttachmentFormat =
      has_depth ? depth_format : VK_FORMAT_UNDEFINED;
  VkGraphicsPipelineCreateInfo info{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = has_depth ? &depth : nullptr;
  info.pColorBlendState = &color;
  info.pDynamicState = &dynamic;
  info.layout = cache->pipeline_layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(state.device_, VK_NULL_HANDLE, 1, &info,
                                nullptr, &pipeline) != VK_SUCCESS) {
    if (error)
      *error = "vkCreateGraphicsPipelines failed for formal Raster";
    return false;
  }
  cache->pipelines.emplace(key, pipeline);
  *out = pipeline;
  *cache_hit = false;
  return true;
}

bool ensure_plan_raster_pipeline(DeviceState &state, VkFormat color_format,
                                 VkFormat depth_format, bool has_depth,
                                 uint32_t sample_count, bool depth_test_enable,
                                 bool depth_write_enable,
                                 core::DepthCompareOp depth_compare_op,
                                 VkPipeline *out, bool *cache_hit,
                                 std::string *error) {
  return ensure_plan_raster_pipeline_impl(
      state, color_format, depth_format, has_depth, sample_count,
      depth_test_enable, depth_write_enable, depth_compare_op, nullptr,
      "builtin", out, cache_hit, error);
}

bool ensure_plan_user_raster_pipeline(
    DeviceState &state, const UserRasterSpirv &program, VkFormat color_format,
    VkFormat depth_format, bool has_depth, uint32_t sample_count,
    bool depth_test_enable, bool depth_write_enable,
    core::DepthCompareOp depth_compare_op, VkPipeline *out, bool *cache_hit,
    std::string *error) {
  auto *cache = ensure_plan_raster_cache(state, error);
  if (cache == nullptr)
    return false;
  auto [it, inserted] = cache->user_modules.try_emplace(program.cache_key);
  UserRasterShaderModules &modules = it->second;
  if (inserted || modules.vertex == VK_NULL_HANDLE ||
      modules.fragment == VK_NULL_HANDLE) {
    if (!create_user_raster_shader_modules(state.device_, program, &modules,
                                           error)) {
      if (inserted)
        cache->user_modules.erase(it);
      return false;
    }
  }
  return ensure_plan_raster_pipeline_impl(
      state, color_format, depth_format, has_depth, sample_count,
      depth_test_enable, depth_write_enable, depth_compare_op, &modules,
      program.cache_key, out, cache_hit, error);
}
#endif

bool compile_plan_raster_package(DeviceState &state,
                                 const core::ExecutionPlan::ResolvedNode &node,
                                 hal::CompiledPlan::PerNodePackage *out,
                                 hal::LoweringReport *report,
                                 std::string *error) {
  if (out == nullptr || report == nullptr) {
    if (error)
      *error = "Raster package output/report is required";
    return false;
  }
  if (node.execution_domain != core::TaskKind::Raster) {
    if (error)
      *error = "Raster package requires a Raster NodeRef";
    return false;
  }
  if (node.user_raster_shader.has_value()) {
    if (!node.code_object ||
        node.code_object->format_tag != "vg.glsl.raster/v1") {
      if (error)
        *error = "Vulkan user Raster requires code object format "
                 "vg.glsl.raster/v1; vg.msl.raster/v1 is Unsupported";
      return false;
    }
#if !defined(VG_HAS_VULKAN)
    UserRasterSpirv unavailable;
    if (!compile_user_raster_glsl(*node.user_raster_shader, &unavailable,
                                  error))
      return false;
#else
    auto *cache = ensure_plan_raster_cache(state, error);
    if (cache == nullptr)
      return false;
    const UserRasterSpirv *program = nullptr;
    if (!get_or_compile_user_raster_glsl(&cache->user_spirv_cache,
                                         *node.user_raster_shader, &program,
                                         error))
      return false;
    if (program == nullptr) {
      if (error)
        *error = "Vulkan user Raster compiler returned no immutable program";
      return false;
    }
#endif
    out->ref = node.ref;
    out->kind = hal::CompiledPlan::NodePackageKind::Raster;
    out->package.reset();
    out->host_assisted = true;
    report->add("node_user_raster_package", hal::LoweringClass::HostAssisted, 1,
                node.user_raster_shader->source.size(),
                "restricted vg.glsl.raster/v1 compiled to immutable two-stage "
                "SPIR-V; formal pipeline creation is device-cache assisted");
    return true;
  }
  if (!node.module.has_value()) {
    if (error)
      *error = "built-in Raster package requires a Raster NodeRef module";
    return false;
  }
  out->ref = node.ref;
  out->kind = hal::CompiledPlan::NodePackageKind::Raster;
  out->package.reset();
  out->host_assisted = false;
  report->add("node_raster_package", hal::LoweringClass::Direct, 1, 0,
              "immutable built-in Raster NodeRef package selected; pipeline "
              "creation is coordinator-integrated");
  return true;
}

bool submit_plan_raster_step(DeviceState &state,
                             const hal::CompiledPlan &compiled,
                             uint32_t task_index,
                             const std::vector<uint32_t> &transitions_before,
                             core::Arena &arena, hal::Submission *submission,
                             PlanRasterStepStats *stats, std::string *error,
                             const GpuDrawCommandView *tier2_view,
                             uint32_t tier2_command) {
  // Validation is intentionally independent of command encoding so the shared
  // submit owner can reject a malformed sealed step before partial effects.
  if (!validate_raster_task(compiled, task_index, arena, state.facet_pool(),
                            error))
    return false;
  const auto &task = compiled.plan.task_graph.tasks()[task_index];
  const auto *node =
      find_node(compiled.plan, {task.node_index, task.node_generation});
  if (node == nullptr ||
      (!node->module.has_value() && !node->user_raster_shader.has_value())) {
    if (error)
      *error = "Raster task has no immutable module or user shader snapshot";
    return false;
  }
  core::FacetRef effective_source = task.raster_facets.source;
  std::array<float, 4> effective_tint = task.raster_tint;
  std::array<uint8_t, 80> effective_root{};
  const std::string &root_schema = node->user_raster_shader.has_value()
                                       ? node->user_raster_shader->root_schema
                                       : node->module->root_schema;
  if (core::is_scene_root_raster_schema(root_schema)) {
    core::ResolvedSceneRootRaster root;
    if (!core::resolve_scene_root_raster(arena, task, &root, error))
      return false;
    effective_source = root.albedo;
    effective_tint = root.base_color;
    std::memcpy(effective_root.data(), root.camera_clip_from_local.data(), 64);
  } else {
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(effective_root.data(), identity, sizeof(identity));
  }
  const bool has_depth = task.depth_attachment_ref.index != 0 ||
                         task.depth_attachment_ref.generation != 0;
#if !defined(VG_HAS_VULKAN)
  if (error)
    *error = "Vulkan adapter is unavailable in this build";
  return false;
#else
  const auto *vertex_slot =
      state.resolve_facet(arena, state.facet_pool(), task.vertex_buffer_ref,
                          core::FacetKind::Address, error);
  const auto *target_slot =
      state.resolve_facet(arena, state.facet_pool(), task.raster_facets.target,
                          core::FacetKind::Attachment, error);
  if (!vertex_slot || !target_slot)
    return false;
  const core::FacetSlot *depth_slot = nullptr;
  if (has_depth) {
    depth_slot = DeviceState::resolve_facet(arena, state.facet_pool(),
                                            task.depth_attachment_ref,
                                            core::FacetKind::Attachment, error);
    if (depth_slot == nullptr)
      return false;
    if (depth_slot->view.format != core::PixelFormat::Depth32Float) {
      if (error)
        *error = "Raster depth attachment must have Depth32Float format";
      return false;
    }
  }
  if (has_depth &&
      (depth_slot->view.width != target_slot->view.width ||
       depth_slot->view.height != target_slot->view.height ||
       depth_slot->view.array_layers != target_slot->view.array_layers ||
       depth_slot->view.mip_levels != target_slot->view.mip_levels)) {
    if (error)
      *error = "Raster depth attachment dimensions and layers must match color";
    return false;
  }
  const auto *vertices = arena.lookup(core::PointerRef{
      vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
  if (!vertices)
    return false;
  auto *cache = ensure_plan_raster_cache(state, error);
  if (!cache)
    return false;
  PlanRasterBindings bindings(state.facet_pool(), task.raster_facets.target,
                              effective_source);
  if (!prepare_plan_raster_bindings(state, task, effective_source,
                                    effective_tint, effective_root, *vertices,
                                    arena, *cache, &bindings, error))
    return false;
  VkPipeline pipeline{};
  bool cache_hit{};
  if (node->user_raster_shader.has_value()) {
    const UserRasterSpirv *program = nullptr;
    if (!get_or_compile_user_raster_glsl(&cache->user_spirv_cache,
                                         *node->user_raster_shader, &program,
                                         error) ||
        program == nullptr ||
        !ensure_plan_user_raster_pipeline(
            state, *program, to_vk_format(target_slot->view.format),
            has_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, has_depth,
            1, task.depth_test_enable, task.depth_write_enable,
            task.depth_compare_op, &pipeline, &cache_hit, error)) {
      destroy_plan_raster_bindings(state, &bindings);
      return false;
    }
  } else if (!ensure_plan_raster_pipeline(
                 state, to_vk_format(target_slot->view.format),
                 has_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED,
                 has_depth, 1, task.depth_test_enable, task.depth_write_enable,
                 task.depth_compare_op, &pipeline, &cache_hit, error)) {
    destroy_plan_raster_bindings(state, &bindings);
    return false;
  }
  if (!ensure_command_pool(state.device_, state.compute_queue_family_,
                           &state.command_pool_, error)) {
    destroy_plan_raster_bindings(state, &bindings);
    return false;
  }
  VkCommandBuffer command{};
  if (!allocate_command_buffer(state.device_, state.command_pool_, &command,
                               error)) {
    destroy_plan_raster_bindings(state, &bindings);
    return false;
  }
  RawBuffer readback{};
  RawBuffer depth_readback{};
  const VkDeviceSize readback_size =
      static_cast<VkDeviceSize>(target_slot->view.width) *
      target_slot->view.height * 4;
  const VkDeviceSize depth_readback_size =
      has_depth ? static_cast<VkDeviceSize>(depth_slot->view.width) *
                      depth_slot->view.height * sizeof(float)
                : 0;
  if (!create_raw_buffer(state.device_, state.physical_device_, readback_size,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true,
                         &readback, error) ||
      (has_depth &&
       !create_raw_buffer(state.device_, state.physical_device_,
                          depth_readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          false, true, &depth_readback, error))) {
    destroy_raw_buffer(state.device_, &depth_readback);
    destroy_raw_buffer(state.device_, &readback);
    destroy_plan_raster_bindings(state, &bindings);
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command, &begin);
  uint64_t barriers = 0;
  for (uint32_t transition : transitions_before) {
    (void)transition;
    VkMemoryBarrier2 task_memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    task_memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    task_memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    task_memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    task_memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                                VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &task_memory;
    vkCmdPipelineBarrier2(command, &dependency);
    ++barriers;
  }
  if (state.record_layout_transition(command, bindings.target,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL))
    ++barriers;
  if (has_depth && state.record_layout_transition(
                       command, bindings.depth,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))
    ++barriers;
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = bindings.target->view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color.float32[3] = 1;
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (has_depth) {
    depth.imageView = bindings.depth->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.0f;
  }
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {target_slot->view.width,
                                 target_slot->view.height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  rendering.pDepthAttachment = has_depth ? &depth : nullptr;
  vkCmdBeginRendering(command, &rendering);
  VkViewport viewport{
      0, 0, float(target_slot->view.width), float(target_slot->view.height),
      0, 1};
  VkRect2D scissor{{0, 0}, {target_slot->view.width, target_slot->view.height}};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &scissor);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          cache->pipeline_layout, 0, 1, &bindings.set, 0,
                          nullptr);
  const VkDeviceSize vertex_offset = 0;
  vkCmdBindVertexBuffers(command, 0, 1, &bindings.vertices.buffer,
                         &vertex_offset);
  if (tier2_view != nullptr) {
    if (task.index_count != 0)
      vkCmdBindIndexBuffer(command, bindings.index.buffer, 0, bindings.index_type);
    if (!record_plan_tier2_draw_consumer(
            command, *tier2_view, task.index_count != 0,
            [](VkCommandBuffer, uint32_t, std::string *) { return true; },
            tier2_command, 1, nullptr, error)) {
      vkCmdEndRendering(command);
      destroy_raw_buffer(state.device_, &depth_readback);
      destroy_raw_buffer(state.device_, &readback);
      destroy_plan_raster_bindings(state, &bindings);
      return false;
    }
  } else if (task.index_count != 0) {
    vkCmdBindIndexBuffer(command, bindings.index.buffer, 0,
                         bindings.index_type);
    vkCmdDrawIndexed(command, task.index_count, 1, 0, 0, 0);
  } else
    vkCmdDraw(
        command,
        static_cast<uint32_t>(vertices->bytes.size() / sizeof(RasterVertexAbi)),
        1, 0, 0);
  vkCmdEndRendering(command);
  if (state.record_layout_transition(command, bindings.target,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
    ++barriers;
  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {target_slot->view.width, target_slot->view.height, 1};
  vkCmdCopyImageToBuffer(command, bindings.target->image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                         1, &copy);
  if (has_depth) {
    if (state.record_layout_transition(command, bindings.depth,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
      ++barriers;
    VkBufferImageCopy depth_copy{};
    depth_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_copy.imageSubresource.layerCount = 1;
    depth_copy.imageExtent = {depth_slot->view.width, depth_slot->view.height,
                              1};
    vkCmdCopyImageToBuffer(command, bindings.depth->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           depth_readback.buffer, 1, &depth_copy);
  }
  VkMemoryBarrier2 host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  host_dependency.memoryBarrierCount = 1;
  host_dependency.pMemoryBarriers = &host_barrier;
  vkCmdPipelineBarrier2(command, &host_dependency);
  ++barriers;
  vkEndCommandBuffer(command);
  const bool submitted = submit_and_wait_simple(
      state.device_, state.compute_queue_, state.command_pool_, command, error);
  if (submitted && submission != nullptr) {
    hal::RasterTaskResult result{};
    result.task_index = task_index;
    result.width = target_slot->view.width;
    result.height = target_slot->view.height;
    result.stored = true;
    result.contents_defined = true;
    const auto pixels = static_cast<const uint8_t *>(readback.mapped);
    const size_t pixel_count =
        static_cast<size_t>(result.width) * result.height;
    result.resolved_rgba.reserve(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i)
      result.resolved_rgba.push_back(
          {pixels[i * 4] / 255.0f, pixels[i * 4 + 1] / 255.0f,
           pixels[i * 4 + 2] / 255.0f, pixels[i * 4 + 3] / 255.0f});
    if (has_depth) {
      const auto *depth_values =
          static_cast<const float *>(depth_readback.mapped);
      result.resolved_depth.assign(depth_values, depth_values + pixel_count);
      auto *depth_allocation = arena.lookup(core::PointerRef{
          depth_slot->view.allocation, depth_slot->view.allocation_generation});
      if (depth_allocation != nullptr &&
          depth_allocation->bytes.size() >= depth_readback_size) {
        std::memcpy(depth_allocation->bytes.data(), depth_values,
                    static_cast<size_t>(depth_readback_size));
        arena.mark_content_modified(*depth_allocation);
        bindings.depth->content_epoch = depth_allocation->content_epoch;
      }
    }
    auto *allocation = arena.lookup(core::PointerRef{
        target_slot->view.allocation, target_slot->view.allocation_generation});
    if (allocation != nullptr && allocation->bytes.size() >= readback_size) {
      std::memcpy(allocation->bytes.data(), pixels,
                  static_cast<size_t>(readback_size));
      arena.mark_content_modified(*allocation);
      bindings.target->content_epoch = allocation->content_epoch;
    }
    submission->raster_results.push_back(std::move(result));
  }
  destroy_raw_buffer(state.device_, &depth_readback);
  destroy_raw_buffer(state.device_, &readback);
  destroy_plan_raster_bindings(state, &bindings);
  if (!submitted)
    return false;
  if (stats) {
    stats->draw_count = 1;
    stats->command_buffer_count = 1;
    stats->encoder_count = 1;
    stats->barrier_count = barriers;
    stats->queue_wait_count = 1;
  }
  return true;
#endif
}
} // namespace vg::vulkan::detail
