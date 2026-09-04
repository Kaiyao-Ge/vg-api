#include "fixture.h"

namespace vg::tests::metal {

// F3 (ADR-043 Decision #4): a restricted-import MSL source, structurally
// matching the exact binding contract run_raster_pass's encoder assumes --
// same VgRasterVertex/VgRasterVaryings struct layout and the same fixed
// buffer/texture/sampler indices raster_facet_metal_source() (the built-in
// shader, src/compiler/compute_package.cpp) uses -- but with caller-chosen
// entry-point names and a fragment body that ignores the sampled texture,
// sampler, and tint buffer entirely, returning solid green. The encoder still
// unconditionally binds all four at their fixed slots regardless of whether
// this function reads them (metal_device_hal.mm), so the source only needs
// to declare parameters at the right indices to be well-formed Metal, not to
// use them. Solid green (0,1,0,1) round-trips RGBA8Unorm quantization exactly
// and can never be produced by the built-in sample*tint formula against the
// non-green source texel pattern run_task_graph_raster/this test fill, so a
// pixel match against it is proof the custom shader itself executed.
std::string user_raster_msl_source(const std::string& vertex_entry, const std::string& fragment_entry) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\n"
      << "using namespace metal;\n\n"
      << "struct VgRasterVertex { packed_float3 position; packed_float2 uv; };\n"
      << "struct VgRasterVaryings { float4 position [[position]]; float2 uv; };\n"
      << "struct VgRasterFragment { float4 color [[color(0)]]; };\n\n"
      << "vertex VgRasterVaryings " << vertex_entry
      << "(device const VgRasterVertex* vertices [[buffer(" << vg::compiler::kRasterVertexBufferIndex << ")]],\n"
      << "                                         uint vid [[vertex_id]]) {\n"
      << "  VgRasterVaryings varyings;\n"
      << "  varyings.position = float4(float3(vertices[vid].position), 1.0f);\n"
      << "  varyings.uv = float2(vertices[vid].uv);\n"
      << "  return varyings;\n"
      << "}\n\n"
      << "fragment VgRasterFragment " << fragment_entry
      << "(VgRasterVaryings varyings [[stage_in]],\n"
      << "                                             texture2d<float, access::sample> tex [[texture("
      << vg::compiler::kRasterTextureIndex << ")]],\n"
      << "                                             sampler samp [[sampler(" << vg::compiler::kRasterSamplerIndex
      << ")]],\n"
      << "                                             constant float4& tint [[buffer("
      << vg::compiler::kRasterTintBufferIndex << ")]]) {\n"
      << "  (void)tex; (void)samp; (void)tint;\n"
      << "  VgRasterFragment result;\n"
      << "  result.color = float4(0.0f, 1.0f, 0.0f, 1.0f);\n"
      << "  return result;\n"
      << "}\n";
  return out.str();
}

// F2 (ADR-043 Decision #3, ADR-046): rasterization is a shape of TaskRecord/
// ExecutionPlan, not a parallel API -- unlike run_basic_raster above (which
// calls run_raster_triangles() directly against a locally-constructed pool),
// this drives a Raster-kind TaskRecord through the same TaskGraphBuilder ->
// seal -> publish -> ExecutionPlan -> compile() -> submit() path
// run_task_tier0 uses for Compute tasks. Facets are acquired against the
// *device's own* facet_pool() (not a local one), because SubmitOps::raster
// resolves task.raster_facets/vertex_buffer_ref against metal.facet_pool()
// during submit(), not against whatever pool the caller happens to hold.
bool run_task_graph_raster(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-graph-raster: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kExtent = 4;
  constexpr uint32_t kBytes = kExtent * kExtent * 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(kBytes);
  auto& target_alloc = arena.allocate(kBytes);
  auto& depth_alloc = arena.allocate(kBytes);
  for (uint32_t y = 0; y < kExtent; ++y) {
    for (uint32_t x = 0; x < kExtent; ++x) {
      const uint64_t texel = (static_cast<uint64_t>(y) * kExtent + x) * 4;
      source_alloc.bytes[texel + 0] = static_cast<uint8_t>(10 + 40 * x);
      source_alloc.bytes[texel + 1] = static_cast<uint8_t>(20 + 40 * y);
      source_alloc.bytes[texel + 2] = static_cast<uint8_t>(30 + 8 * x + 16 * y);
      source_alloc.bytes[texel + 3] = 255;
    }
  }

  const vg::core::CanonicalView source_view = make_rgba8_view(source_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView depth_view = make_depth32_view(depth_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetRef source_ref;
  vg::core::FacetRef target_ref;
  vg::core::FacetRef depth_ref;
  std::string error;
  if (!metal_device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, depth_view, vg::core::FacetKind::Attachment, &depth_ref, &error)) {
    std::cerr << "task-graph-raster: acquire failed: " << error << "\n";
    return false;
  }

  const auto quad = metal_fullscreen_quad();
  const uint64_t vertex_bytes = quad.size() * sizeof(vg::metal::RasterVertex);
  auto& vertex_alloc = arena.allocate(vertex_bytes);
  std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
  const vg::core::CanonicalView vertex_view =
      make_rgba8_view(vertex_alloc, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
  vg::core::FacetRef vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "task-graph-raster: vertex facet acquire failed: " << error << "\n";
    return false;
  }

  TaskRecord raster_task{};
  raster_task.kind = vg::core::TaskKind::Raster;
  raster_task.raster_facets = {.source = source_ref, .target = target_ref};
  raster_task.vertex_buffer_ref = vertex_ref;
  raster_task.depth_attachment_ref = depth_ref;
  raster_task.depth_test_enable = true;
  raster_task.depth_write_enable = true;
  raster_task.depth_compare_op = vg::core::DepthCompareOp::Less;
  raster_task.raster_filter = vg::core::FilterMode::Nearest;
  raster_task.raster_wrap = vg::core::WrapMode::Clamp;

  const auto module = make_probe_module(arena);
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &metal_device->facet_pool();
  if (!assemble_compute_plan(arena, module, {raster_task}, &plan, &error,
                             raster_options)) {
    std::cerr << "task-graph-raster: plan assembly failed: " << error << "\n";
    return false;
  }

  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-graph-raster: Metal compile failed: " << error << "\n";
    return false;
  }
  if (compiled.per_node_packages.size() != 1 ||
      compiled.per_node_packages[0].kind != vg::hal::CompiledPlan::NodePackageKind::Raster ||
      compiled.per_node_packages[0].package.has_value()) {
    std::cerr << "task-graph-raster: canonical raster Node was compiled as compute\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "task-graph-raster: Metal submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "task-graph-raster: Metal execution reported failure: " << submission.result.message << "\n";
    return false;
  }
  if (submission.published_tasks.size() != 1 ||
      submission.published_tasks[0].kind != vg::core::TaskKind::Raster ||
      submission.published_tasks[0].node_index != plan.task_graph.tasks()[0].node_index) {
    std::cerr << "task-graph-raster: complete canonical publication omitted or changed the Raster Task\n";
    return false;
  }
  if (submission.result.trace.size() != plan.task_effects[0].size() ||
      std::ranges::any_of(submission.result.trace, [&](const vg::ir::Effect& effect) {
        return effect.allocation == module.instructions[0].allocation;
      })) {
    std::cerr << "task-graph-raster: canonical module was executed as a compute pre-pass\n";
    return false;
  }
  const auto lifetime_released = [&](const char* phase) {
    for (const auto ref : {source_ref, target_ref, vertex_ref, depth_ref}) {
      if (metal_device->facet_pool().in_flight(ref) != 0) {
        std::cerr << "task-graph-raster: " << phase << " leaked a facet lifetime hold\n";
        return false;
      }
    }
    for (const auto* allocation : {&source_alloc, &target_alloc, &vertex_alloc, &depth_alloc}) {
      if (allocation->in_flight != 0) {
        std::cerr << "task-graph-raster: " << phase << " leaked an allocation lifetime hold\n";
        return false;
      }
    }
    return true;
  };
  if (!lifetime_released("successful submit")) return false;
  vg::hal::Submission repeated_submission;
  if (!metal_device->submit(compiled, arena, &repeated_submission, &error) ||
      !repeated_submission.result.ok || !lifetime_released("repeat submit")) {
    std::cerr << "task-graph-raster: repeat submit/lifetime release failed: " << error << "\n";
    return false;
  }
  if (submission.raster_results.size() != 1) {
    std::cerr << "task-graph-raster: expected exactly one raster_results entry, got "
              << submission.raster_results.size() << "\n";
    return false;
  }
  const auto& raster_result = submission.raster_results[0];
  if (raster_result.task_index != 0 || raster_result.width != kExtent || raster_result.height != kExtent) {
    std::cerr << "task-graph-raster: raster_results[0] shape mismatch\n";
    return false;
  }
  if (raster_result.resolved_depth.size() != static_cast<size_t>(kExtent) * kExtent) {
    std::cerr << "task-graph-raster: depth readback missing\n";
    return false;
  }

  // F2's fixed attachment defaults (load=Clear, store=Store, clear_rgba
  // {0,0,0,1}, sample_count=1, subresource {0,0}) are hard-coded inside
  // submit(); only filter/wrap/tint travel through the TaskRecord. Mirror
  // both here so the oracle call matches exactly what submit() ran.
  vg::metal::RasterDesc oracle_desc;
  oracle_desc.filter = raster_task.raster_filter;
  oracle_desc.wrap = raster_task.raster_wrap;
  oracle_desc.attachment = vg::hal::f2_default_raster_attachment_config<vg::metal::AttachmentFacetDesc>();
  oracle_desc.depth_attachment_ref = depth_ref;
  oracle_desc.depth_test_enable = true;
  oracle_desc.depth_write_enable = true;
  oracle_desc.depth_compare_op = vg::core::DepthCompareOp::Less;
  auto oracle = vg::reference::raster_triangles(arena, metal_device->facet_pool(),
                                                {.source = source_ref, .target = target_ref},
                                                to_reference_desc(oracle_desc), to_reference_vertices(quad));
  if (!oracle.ok) {
    std::cerr << "task-graph-raster: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (raster_result.resolved_depth.size() != oracle.resolved_depth.size()) {
    std::cerr << "task-graph-raster: depth size mismatch\n";
    return false;
  }
  for (size_t i = 0; i < raster_result.resolved_depth.size(); ++i)
    if (std::fabs(raster_result.resolved_depth[i] - oracle.resolved_depth[i]) > kNearestTol) {
      std::cerr << "task-graph-raster: depth mismatch " << raster_result.resolved_depth[i] << " vs "
                << oracle.resolved_depth[i] << "\n";
      return false;
    }
  if (raster_result.resolved_rgba.size() != oracle.resolved_rgba.size()) {
    std::cerr << "task-graph-raster: resolved image size mismatch\n";
    return false;
  }
  for (uint32_t y = 1; y + 1 < kExtent; ++y) {
    for (uint32_t x = 1; x + 1 < kExtent; ++x) {
      const size_t index = static_cast<size_t>(y) * kExtent + x;
      if (!channels_close(raster_result.resolved_rgba[index], oracle.resolved_rgba[index], kNearestTol,
                          "task-graph-raster", "interior pixel"))
        return false;
    }
  }

  // F5: four vertices plus six indices prove Metal did not silently retain
  // drawPrimitives. Both element widths must match the Reference oracle.
  const std::vector<vg::metal::RasterVertex> indexed_vertices{quad[0], quad[1], quad[2], quad[4]};
  auto& indexed_vertex_alloc = arena.allocate(indexed_vertices.size() * sizeof(vg::metal::RasterVertex));
  std::memcpy(indexed_vertex_alloc.bytes.data(), indexed_vertices.data(), indexed_vertex_alloc.bytes.size());
  const auto indexed_vertex_view = make_rgba8_view(
      indexed_vertex_alloc, {.width = static_cast<uint32_t>(indexed_vertex_alloc.bytes.size() / 4), .height = 1});
  vg::core::FacetRef indexed_vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, indexed_vertex_view, vg::core::FacetKind::Address,
                                          &indexed_vertex_ref, &error)) return false;
  const auto run_indexed = [&](const void* bytes, size_t byte_count, vg::core::PixelFormat format,
                               const char* label) {
    auto& index_alloc = arena.allocate(byte_count);
    std::memcpy(index_alloc.bytes.data(), bytes, byte_count);
    auto index_view = make_rgba8_view(index_alloc, {.width = 6, .height = 1});
    index_view.format = format;
    vg::core::FacetRef index_ref;
    if (!metal_device->facet_pool().acquire(arena, index_view, vg::core::FacetKind::Address, &index_ref, &error)) return false;
    vg::core::FacetRef indexed_depth_ref;
    if (!metal_device->facet_pool().acquire(arena, depth_view, vg::core::FacetKind::Attachment,
                                            &indexed_depth_ref, &error)) return false;
    TaskRecord indexed_task = raster_task;
    indexed_task.vertex_buffer_ref = indexed_vertex_ref;
    indexed_task.index_buffer_ref = index_ref;
    indexed_task.index_count = 6;
    indexed_task.depth_attachment_ref = indexed_depth_ref;
    vg::core::ExecutionPlan indexed_plan;
    if (!assemble_compute_plan(arena, module, {indexed_task}, &indexed_plan, &error,
                               raster_options)) return false;
    vg::hal::CompiledPlan indexed_compiled;
    vg::hal::Submission indexed_submission;
    if (!metal_device->compile(indexed_plan, &indexed_compiled, &error) ||
        !metal_device->submit(indexed_compiled, arena, &indexed_submission, &error) || !indexed_submission.result.ok ||
        indexed_submission.raster_results.size() != 1) return false;
    for (const auto ref : {source_ref, target_ref, indexed_vertex_ref, index_ref, indexed_depth_ref})
      if (metal_device->facet_pool().in_flight(ref) != 0) {
        error = std::string(label) + ": leaked a facet lifetime hold";
        return false;
      }
    for (const auto* allocation : {&source_alloc, &target_alloc, &indexed_vertex_alloc,
                                   &index_alloc, &depth_alloc})
      if (allocation->in_flight != 0) {
        error = std::string(label) + ": leaked an allocation lifetime hold";
        return false;
      }
    const auto& actual = indexed_submission.raster_results[0].resolved_rgba;
    if (actual.size() != oracle.resolved_rgba.size()) { error = std::string(label) + ": color size"; return false; }
    for (size_t i = 0; i < actual.size(); ++i)
      if (!channels_close(actual[i], oracle.resolved_rgba[i], kNearestTol, label, "full indexed image")) return false;
    const auto& actual_depth = indexed_submission.raster_results[0].resolved_depth;
    if (actual_depth.size() != oracle.resolved_depth.size()) { error = std::string(label) + ": depth size"; return false; }
    for (size_t i = 0; i < actual_depth.size(); ++i)
      if (std::fabs(actual_depth[i] - oracle.resolved_depth[i]) > kNearestTol) {
        error = std::string(label) + ": depth mismatch";
        return false;
      }
    return true;
  };
  const std::array<uint16_t, 6> indices16{0, 1, 2, 2, 1, 3};
  const std::array<uint32_t, 6> indices32{0, 1, 2, 2, 1, 3};
  if (!run_indexed(indices16.data(), sizeof(indices16), vg::core::PixelFormat::R16Uint, "indexed-u16") ||
      !run_indexed(indices32.data(), sizeof(indices32), vg::core::PixelFormat::R32Uint, "indexed-u32")) {
    std::cerr << "task-graph-raster: indexed Metal/reference differential failed: " << error << "\n";
    return false;
  }

  // A resolved NodeRef cannot serve both execution domains. This is distinct
  // from a legitimate canonical mixed-domain plan (one Node per domain), which
  // Metal now executes through the sealed schedule. Restricted user shading
  // retains its separate Stage-6 mixed-domain rejection.
  const auto& mixed_root = arena.allocate(4);
  TaskRecord compute_task{};
  compute_task.root_allocation = mixed_root.id;
  compute_task.root_generation = mixed_root.generation;
  compute_task.x = 2;
  compute_task.y = 1;
  compute_task.z = 1;
  TaskRecord mixed_raster_task = raster_task;
  vg::test_support::AssemblyOptions mixed_options;
  mixed_options.timeline_signal = 7;
  mixed_options.facet_pool = &metal_device->facet_pool();
  vg::core::ExecutionPlan mixed_plan;
  std::string mixed_error;
  if (assemble_compute_plan(arena, module, {compute_task, mixed_raster_task}, &mixed_plan, &mixed_error,
                            mixed_options)) {
    std::cerr << "task-graph-raster: semantic assembly accepted a NodeRef shared across execution domains\n";
    return false;
  }
  if (mixed_error != "one resolved NodeRef is used by multiple execution domains") {
    std::cerr << "task-graph-raster: unexpected shared-NodeRef rejection: " << mixed_error << "\n";
    return false;
  }
  if (mixed_root.in_flight != 0) {
    std::cerr << "task-graph-raster: rejected shared NodeRef acquired a lifetime hold\n";
    return false;
  }

  std::cout << "task-graph-raster: ok\n";
  return true;
}

// F4: one public-task-shaped raster submission with two fully overlapping
// triangles in one triangle list. The first samples the red source texel at
// z=.75; the second samples green at z=.25. Less+write therefore makes the
// center green. Keeping both triangles in one task deliberately exercises the
// per-task clear=1.0 rule rather than assuming depth carries across tasks.
bool run_task_graph_raster_depth(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-graph-raster-depth: no Metal device available on this host\n";
    return false;
  }
  constexpr uint32_t kExtent = 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(2 * 4);
  source_alloc.bytes = {255, 0, 0, 255, 0, 255, 0, 255};
  auto& target_alloc = arena.allocate(kExtent * kExtent * 4);
  auto& depth_alloc = arena.allocate(kExtent * kExtent * 4);
  const auto source_view = make_rgba8_view(source_alloc, {.width = 2, .height = 1});
  const auto target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});
  const auto depth_view = make_depth32_view(depth_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetRef source_ref, target_ref, depth_ref;
  std::string error;
  if (!metal_device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, depth_view, vg::core::FacetKind::Attachment, &depth_ref, &error)) {
    std::cerr << "task-graph-raster-depth: facet acquire failed: " << error << "\n";
    return false;
  }
  const auto tri = [](float z, float u) {
    return std::array<vg::metal::RasterVertex, 3>{{
        {-1.0f, 1.0f, z, u, 0.5f}, {3.0f, 1.0f, z, u, 0.5f}, {-1.0f, -3.0f, z, u, 0.5f}}};
  };
  std::vector<vg::metal::RasterVertex> vertices;
  const auto far = tri(0.75f, 0.25f);
  const auto near = tri(0.25f, 0.75f);
  vertices.insert(vertices.end(), far.begin(), far.end());
  vertices.insert(vertices.end(), near.begin(), near.end());
  auto& vertex_alloc = arena.allocate(vertices.size() * sizeof(vg::metal::RasterVertex));
  std::memcpy(vertex_alloc.bytes.data(), vertices.data(), vertex_alloc.bytes.size());
  const auto vertex_view = make_rgba8_view(
      vertex_alloc, {.width = static_cast<uint32_t>(vertex_alloc.bytes.size() / 4), .height = 1});
  vg::core::FacetRef vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "task-graph-raster-depth: vertex facet acquire failed: " << error << "\n";
    return false;
  }
  TaskRecord task{};
  task.kind = vg::core::TaskKind::Raster;
  task.raster_facets = {.source = source_ref, .target = target_ref};
  task.depth_attachment_ref = depth_ref;
  task.depth_test_enable = true;
  task.depth_write_enable = true;
  task.depth_compare_op = vg::core::DepthCompareOp::Less;
  task.vertex_buffer_ref = vertex_ref;
  task.raster_filter = vg::core::FilterMode::Nearest;
  task.raster_wrap = vg::core::WrapMode::Clamp;
  const auto module = make_probe_module(arena);
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &metal_device->facet_pool();
  if (!assemble_compute_plan(arena, module, {task}, &plan, &error,
                             raster_options)) {
    std::cerr << "task-graph-raster-depth: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-graph-raster-depth: Metal compile failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error) || !submission.result.ok ||
      submission.raster_results.size() != 1) {
    std::cerr << "task-graph-raster-depth: submit failed: "
              << (error.empty() ? submission.result.message : error) << "\n";
    return false;
  }
  const auto& center = submission.raster_results[0].resolved_rgba[(kExtent / 2) * kExtent + kExtent / 2];
  if (!channels_close(center, {0.0f, 1.0f, 0.0f, 1.0f}, kNearestTol,
                      "task-graph-raster-depth", "near triangle wins Less test"))
    return false;

  // An RGBA attachment may not masquerade as depth. Validation can happen in
  // compile or submit; either route is acceptable, but it must not succeed.
  TaskRecord invalid = task;
  invalid.depth_attachment_ref = target_ref;
  TaskGraphBuilder invalid_builder;
  TaskGraph invalid_graph;
  if (!invalid_builder.append(invalid) || !invalid_builder.seal(&invalid_graph) || !invalid_graph.publish()) return false;
  plan.task_graph = invalid_graph;
  plan.graph_epoch = arena.topology_epoch();
  vg::hal::CompiledPlan invalid_compiled;
  if (metal_device->compile(plan, &invalid_compiled, &error)) {
    vg::hal::Submission invalid_submission;
    if (!metal_device->submit(invalid_compiled, arena, &invalid_submission, &error) || invalid_submission.result.ok) {
      std::cerr << "task-graph-raster-depth: RGBA depth attachment was accepted\n";
      return false;
    }
  }
  std::cout << "task-graph-raster-depth: ok\n";
  return true;
}

// F3 (ADR-043 Decision #4): restricted-import "vg.msl.raster/v1" shaders --
// the resolved Node owns a user raster contract rather than canonical compute
// IR. Three sub-cases:
//   (a) happy path: a real, hand-written MSL vertex+fragment pair matching
//       the exact binding contract run_raster_pass's encoder assumes
//       (user_raster_msl_source above) must actually execute -- every
//       resolved pixel must match the custom shader's own solid-green
//       formula, not the built-in sample*tint formula, and compile() must
//       record a "raster_user_shader"/HostAssisted disclosure event
//       (docs/START.md invariant 10: no silent "verified" reclassification --
//       see also reference_raster_test.cpp's equivalent HostAssisted check).
//   (b) malformed entry point: fragment_entry names a function absent from
//       source. ensure_raster_pipeline compiles the MTLLibrary/pipeline
//       lazily at submit() time, not at compile() time, so compile() must
//       still succeed; submit() itself must still return true (host-side
//       acceptance), but submission.result.ok must be false with a message
//       containing "Metal raster pipeline compile failed" -- a clean
//       submit-time failure, never a crash or a silent fallback to the
//       built-in shader.
//   (c) mixed compute+MSL-raster rejection: ExecutionPlan::validate()
//       requires every task to be Raster-kind whenever user_raster_shader is
//       set (device_hal.cpp); a graph mixing a Compute task with a Raster
//       task must be rejected at compile() with that exact message.
bool run_task_graph_raster_user_shader(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-graph-raster-user-shader: no Metal device available on this host\n";
    return false;
  }
  // Note: unlike the reference backend (reference_device_hal.cpp), Metal's
  // capabilities() does not currently OR in Capability::UserShaderImport even
  // though compile()/submit() fully implement it -- see the final report's
  // flagged-bug list. Not asserted here since it is not part of this
  // sub-case's required behaviour and the plan is submitted directly.

  constexpr uint32_t kExtent = 4;
  constexpr uint32_t kBytes = kExtent * kExtent * 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(kBytes);
  auto& target_alloc = arena.allocate(kBytes);
  for (uint32_t y = 0; y < kExtent; ++y) {
    for (uint32_t x = 0; x < kExtent; ++x) {
      const uint64_t texel = (static_cast<uint64_t>(y) * kExtent + x) * 4;
      source_alloc.bytes[texel + 0] = static_cast<uint8_t>(10 + 40 * x);
      source_alloc.bytes[texel + 1] = static_cast<uint8_t>(20 + 40 * y);
      source_alloc.bytes[texel + 2] = static_cast<uint8_t>(30 + 8 * x + 16 * y);
      source_alloc.bytes[texel + 3] = 255;
    }
  }

  const vg::core::CanonicalView source_view = make_rgba8_view(source_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetRef source_ref;
  vg::core::FacetRef target_ref;
  std::string error;
  if (!metal_device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref,
                                          &error)) {
    std::cerr << "task-graph-raster-user-shader: acquire failed: " << error << "\n";
    return false;
  }

  const auto quad = metal_fullscreen_quad();
  const uint64_t vertex_bytes = quad.size() * sizeof(vg::metal::RasterVertex);
  auto& vertex_alloc = arena.allocate(vertex_bytes);
  std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
  const vg::core::CanonicalView vertex_view =
      make_rgba8_view(vertex_alloc, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
  vg::core::FacetRef vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "task-graph-raster-user-shader: vertex facet acquire failed: " << error << "\n";
    return false;
  }

  TaskRecord raster_task{};
  raster_task.kind = vg::core::TaskKind::Raster;
  raster_task.raster_facets = {.source = source_ref, .target = target_ref};
  raster_task.vertex_buffer_ref = vertex_ref;
  raster_task.raster_filter = vg::core::FilterMode::Nearest;
  raster_task.raster_wrap = vg::core::WrapMode::Clamp;

  TaskGraphBuilder builder;
  if (!builder.append(raster_task)) {
    std::cerr << "task-graph-raster-user-shader: failed to append raster task\n";
    return false;
  }
  TaskGraph graph;
  if (!builder.seal(&graph) || !graph.publish()) {
    std::cerr << "task-graph-raster-user-shader: failed to seal/publish task graph\n";
    return false;
  }

  // (a) Happy path: a real, valid custom shader whose own formula (solid
  // green) is trivially distinguishable from the built-in sample*tint
  // formula against this non-green source texel pattern.
  const vg::ir::UserRasterShaderContract shader{
      "vg.test.raster/v1", "vg_user_raster_vertex", "vg_user_raster_fragment",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      user_raster_msl_source("vg_user_raster_vertex", "vg_user_raster_fragment")};
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &metal_device->facet_pool();
  if (!assemble_user_raster_plan(arena, shader, {raster_task}, &plan, &error,
                                 raster_options)) {
    std::cerr << "task-graph-raster-user-shader: plan assembly failed: " << error << "\n";
    return false;
  }

  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-graph-raster-user-shader: Metal compile failed: " << error << "\n";
    return false;
  }
  if (!compiled.report.supported) {
    std::cerr << "task-graph-raster-user-shader: report.supported should be true\n";
    return false;
  }
  bool found_user_shader_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation == "raster_user_shader" && event.classification == vg::hal::LoweringClass::HostAssisted)
      found_user_shader_event = true;
  }
  if (!found_user_shader_event) {
    std::cerr << "task-graph-raster-user-shader: missing HostAssisted raster_user_shader LoweringEvent\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "task-graph-raster-user-shader: Metal submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "task-graph-raster-user-shader: Metal execution reported failure: " << submission.result.message
              << "\n";
    return false;
  }
  if (submission.raster_results.size() != 1) {
    std::cerr << "task-graph-raster-user-shader: expected exactly one raster_results entry, got "
              << submission.raster_results.size() << "\n";
    return false;
  }
  const auto& raster_result = submission.raster_results[0];
  if (raster_result.width != kExtent || raster_result.height != kExtent) {
    std::cerr << "task-graph-raster-user-shader: raster_results[0] shape mismatch\n";
    return false;
  }
  const std::array<float, 4> solid_green{0.0f, 1.0f, 0.0f, 1.0f};
  for (size_t index = 0; index < raster_result.resolved_rgba.size(); ++index) {
    if (!channels_close(raster_result.resolved_rgba[index], solid_green, kNearestTol,
                        "task-graph-raster-user-shader", "custom-shader pixel"))
      return false;
  }

  // (b) Malformed entry point: fragment_entry names a function absent from
  // source. compile() still succeeds (pipeline is built lazily at submit()),
  // but submit() must report a clean, non-crashing failure via
  // submission.result.
  const vg::ir::UserRasterShaderContract bad_shader{
      "vg.test.raster/v1", "vg_user_raster_vertex", "vg_does_not_exist_in_source",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      user_raster_msl_source("vg_user_raster_vertex", "vg_user_raster_fragment")};
  vg::core::ExecutionPlan bad_plan;
  if (!assemble_user_raster_plan(arena, bad_shader, {raster_task}, &bad_plan, &error,
                                 raster_options)) {
    std::cerr << "task-graph-raster-user-shader: bad-plan assembly failed: " << error << "\n";
    return false;
  }

  vg::hal::CompiledPlan bad_compiled;
  std::string bad_compile_error;
  if (!metal_device->compile(bad_plan, &bad_compiled, &bad_compile_error)) {
    std::cerr << "task-graph-raster-user-shader: compile() should defer pipeline compilation to submit(), "
                 "but failed at compile() instead: "
              << bad_compile_error << "\n";
    return false;
  }
  vg::hal::Submission bad_submission;
  std::string bad_submit_error;
  if (!metal_device->submit(bad_compiled, arena, &bad_submission, &bad_submit_error)) {
    std::cerr << "task-graph-raster-user-shader: submit() call itself should succeed even when the "
                 "pipeline fails to compile (host-side acceptance), but failed: "
              << bad_submit_error << "\n";
    return false;
  }
  if (bad_submission.result.ok) {
    std::cerr << "task-graph-raster-user-shader: malformed entry point must report submission.result.ok "
                 "== false\n";
    return false;
  }
  if (bad_submission.result.message.find("Metal raster pipeline compile failed") == std::string::npos) {
    std::cerr << "task-graph-raster-user-shader: unexpected malformed-entry-point failure message: "
              << bad_submission.result.message << "\n";
    return false;
  }

  // (c) Mixed compute+MSL-raster rejection: validate() requires every task
  // to be Raster-kind whenever user_raster_shader is set.
  const auto& user_shader_mixed_root = arena.allocate(4);
  TaskRecord compute_task{};
  compute_task.root_allocation = user_shader_mixed_root.id;
  compute_task.root_generation = user_shader_mixed_root.generation;
  compute_task.x = 1;
  compute_task.y = 1;
  compute_task.z = 1;
  TaskRecord mixed_raster_task{};
  mixed_raster_task.node_index = 1;
  mixed_raster_task.kind = vg::core::TaskKind::Raster;
  mixed_raster_task.raster_facets = {.source = source_ref, .target = target_ref};
  mixed_raster_task.vertex_buffer_ref = vertex_ref;
  TaskGraphBuilder mixed_builder;
  if (!mixed_builder.append(compute_task) || !mixed_builder.append(mixed_raster_task)) {
    std::cerr << "task-graph-raster-user-shader: failed to build mixed compute+raster graph\n";
    return false;
  }
  TaskGraph mixed_graph;
  if (!mixed_builder.seal(&mixed_graph) || !mixed_graph.publish()) {
    std::cerr << "task-graph-raster-user-shader: failed to seal/publish mixed compute+raster graph\n";
    return false;
  }
  std::string mixed_error;
  vg::core::ExecutionPlan mixed_plan;
  if (assemble_user_raster_plan(arena, shader, {compute_task, mixed_raster_task},
                                &mixed_plan, &mixed_error, raster_options)) {
    std::cerr << "task-graph-raster-user-shader: assembly unexpectedly accepted a mixed compute+"
                 "user_raster_shader graph\n";
    return false;
  }
  if (mixed_error != "task kind does not match its resolved node execution domain") {
    std::cerr << "task-graph-raster-user-shader: unexpected mixed-graph rejection message: " << mixed_error
              << "\n";
    return false;
  }

  std::cout << "task-graph-raster-user-shader: ok\n";
  return true;
}

}  // namespace vg::tests::metal
