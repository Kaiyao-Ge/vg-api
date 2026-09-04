#include "raster_fixture.h"

namespace vg::tests::reference {

void test_builtin_raster_submit() {
  // --- F2 (ADR-043 Decision #3, ADR-046): rasterization is a shape of
  // TaskRecord/ExecutionPlan, not a parallel API. A Raster-kind task driven
  // through TaskGraphBuilder -> seal -> publish -> ExecutionPlan ->
  // compile() -> submit() must land in submission.raster_results with the
  // exact same result raster_triangles() would produce called directly.
  // Facets are acquired against the device's own facet_pool() (not a local
  // one), since submit() resolves task.raster_facets/vertex_buffer_ref
  // against that pool, not a caller-supplied one. ---
  {
    auto device = vg::reference::make_device_hal();
    assert(device != nullptr);

    vg::core::Arena arena;
    constexpr uint32_t kExtent = 4;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = kExtent, .height = kExtent});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = kExtent, .height = kExtent});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    for (uint32_t y = 0; y < kExtent; ++y)
      for (uint32_t x = 0; x < kExtent; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    std::string error;
    vg::core::FacetRef source_ref, target_ref;
    assert(device->facet_pool().acquire(arena, source, vg::core::FacetKind::Sample, &source_ref, &error));
    assert(device->facet_pool().acquire(arena, target, vg::core::FacetKind::Attachment, &target_ref, &error));

    const auto quad = full_target_quad();
    const uint64_t vertex_bytes = quad.size() * sizeof(vg::reference::RasterVertex);
    auto& vertex_alloc = arena.allocate(vertex_bytes);
    std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
    const vg::core::CanonicalView vertex_view =
        plain_view(vertex_alloc.id, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
    vg::core::FacetRef vertex_ref;
    assert(device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error));

    vg::core::TaskRecord raster_task{};
    raster_task.kind = vg::core::TaskKind::Raster;
    raster_task.raster_facets = {.source = source_ref, .target = target_ref};
    raster_task.vertex_buffer_ref = vertex_ref;
    raster_task.raster_filter = vg::core::FilterMode::Nearest;
    raster_task.raster_wrap = vg::core::WrapMode::Clamp;

    const auto module = probe_module(arena);
    vg::test_support::AssembledPlanFixture fixture;
    vg::core::ExecutionPlan plan;
    vg::test_support::AssemblyOptions raster_options;
    raster_options.facet_pool = &device->facet_pool();
    assert(vg::test_support::assemble_single_node_plan(arena, module, {raster_task},
                                                        &fixture, &plan, &error,
                                                        raster_options));

    vg::hal::CompiledPlan compiled;
    assert(device->compile(plan, &compiled, &error));
    assert(compiled.per_node_packages.size() == 1);
    assert(compiled.per_node_packages[0].kind ==
           vg::hal::CompiledPlan::NodePackageKind::Raster);
    assert(!compiled.per_node_packages[0].package.has_value());

    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    assert(submission.result.trace.size() == plan.task_effects[0].size());
    assert(std::ranges::none_of(submission.result.trace, [&](const vg::ir::Effect& effect) {
      return effect.allocation == module.instructions[0].allocation;
    }));
    assert(submission.raster_results.size() == 1);
    const auto& task_result = submission.raster_results[0];
    assert(task_result.task_index == 0);
    assert(task_result.width == kExtent);
    assert(task_result.height == kExtent);

    // F2's fixed attachment defaults are hard-coded inside submit(); mirror
    // them here so the direct call matches exactly what submit() ran.
    vg::reference::RasterDesc oracle_desc;
    oracle_desc.filter = raster_task.raster_filter;
    oracle_desc.wrap = raster_task.raster_wrap;
    oracle_desc.attachment =
        vg::hal::f2_default_raster_attachment_config<vg::reference::AttachmentFacetDesc>();
    const auto oracle = vg::reference::raster_triangles(arena, device->facet_pool(), {.source = source_ref, .target = target_ref},
                                                        oracle_desc, quad);
    assert(oracle.ok);
    assert(task_result.resolved_rgba.size() == oracle.resolved_rgba.size());
    assert(task_result.stored == oracle.stored);
    assert(task_result.contents_defined == oracle.contents_defined);
    for (size_t i = 0; i < oracle.resolved_rgba.size(); ++i) assert(exact_match(task_result.resolved_rgba[i], oracle.resolved_rgba[i]));

    // F5: a u16 Address facet selects indexed draw without a public vertex
    // descriptor. Reordering the quad indices exercises vertex-id indirection
    // while the Reference oracle remains the same raster implementation.
    const std::array<uint16_t, 6> indices{0, 1, 2, 3, 4, 5};
    auto& index_alloc = arena.allocate(sizeof(indices));
    std::memcpy(index_alloc.bytes.data(), indices.data(), sizeof(indices));
    auto index_view = plain_view(index_alloc.id, {.width = static_cast<uint32_t>(indices.size()), .height = 1});
    index_view.format = vg::core::PixelFormat::R16Uint;
    vg::core::FacetRef index_ref;
    assert(device->facet_pool().acquire(arena, index_view, vg::core::FacetKind::Address, &index_ref, &error));
    vg::core::TaskRecord indexed_task = raster_task;
    indexed_task.index_buffer_ref = index_ref;
    indexed_task.index_count = static_cast<uint32_t>(indices.size());
    vg::test_support::AssembledPlanFixture indexed_fixture;
    vg::core::ExecutionPlan indexed_plan;
    assert(vg::test_support::assemble_single_node_plan(arena, module, {indexed_task},
                                                        &indexed_fixture, &indexed_plan, &error,
                                                        raster_options));
    vg::hal::CompiledPlan indexed_compiled;
    std::string indexed_error;
    assert(device->compile(indexed_plan, &indexed_compiled, &indexed_error));
    vg::hal::Submission indexed_submission;
    assert(device->submit(indexed_compiled, arena, &indexed_submission, &indexed_error));
    assert(indexed_submission.result.ok);
    assert(indexed_submission.raster_results.size() == 1);

    // A two-Task raster submission preserves Task 0's produced result when
    // Task 1 discovers an invalid index during actual raster execution.
    const std::array<uint16_t, 3> bad_indices{99, 99, 99};
    auto& bad_index_alloc = arena.allocate(sizeof(bad_indices));
    std::memcpy(bad_index_alloc.bytes.data(), bad_indices.data(), sizeof(bad_indices));
    auto bad_index_view = plain_view(bad_index_alloc.id,
                                     {.width = static_cast<uint32_t>(bad_indices.size()), .height = 1});
    bad_index_view.format = vg::core::PixelFormat::R16Uint;
    vg::core::FacetRef bad_index_ref;
    assert(device->facet_pool().acquire(arena, bad_index_view, vg::core::FacetKind::Address,
                                        &bad_index_ref, &indexed_error));
    auto failing_raster_task = raster_task;
    failing_raster_task.index_buffer_ref = bad_index_ref;
    failing_raster_task.index_count = static_cast<uint32_t>(bad_indices.size());
    vg::test_support::AssembledPlanFixture partial_fixture;
    vg::core::ExecutionPlan partial_plan;
    assert(vg::test_support::assemble_single_node_plan(
        arena, module, {raster_task, failing_raster_task}, &partial_fixture, &partial_plan,
        &indexed_error, raster_options));
    vg::hal::CompiledPlan partial_compiled;
    assert(device->compile(partial_plan, &partial_compiled, &indexed_error));
    vg::hal::Submission partial_submission;
    assert(device->submit(partial_compiled, arena, &partial_submission, &indexed_error));
    assert(!partial_submission.result.ok);
    assert(!partial_submission.result.outputs_valid);
    assert(partial_submission.result.poison == vg::core::PoisonState::PartiallyProduced);
    assert(partial_submission.result.fault.task_index == 1);
    assert(!partial_submission.result.fault.code.empty());
    assert(!partial_submission.result.fault.message.empty());
    assert(partial_submission.raster_results.size() == 1);
    assert(partial_submission.raster_results[0].task_index == 0);
    assert(!partial_submission.result.trace.empty());
    assert(!partial_submission.result.witness.entries().empty());

    // The identical stream encoded as u32 takes the other F5 decode path.
    const std::array<uint32_t, 6> wide_indices{0, 1, 2, 3, 4, 5};
    auto& wide_index_alloc = arena.allocate(sizeof(wide_indices));
    std::memcpy(wide_index_alloc.bytes.data(), wide_indices.data(), sizeof(wide_indices));
    auto wide_index_view = plain_view(wide_index_alloc.id,
                                      {.width = static_cast<uint32_t>(wide_indices.size()), .height = 1});
    wide_index_view.format = vg::core::PixelFormat::R32Uint;
    vg::core::FacetRef wide_index_ref;
    assert(device->facet_pool().acquire(arena, wide_index_view, vg::core::FacetKind::Address, &wide_index_ref, &error));
    indexed_task.index_buffer_ref = wide_index_ref;
    vg::test_support::AssembledPlanFixture wide_index_fixture;
    assert(vg::test_support::assemble_single_node_plan(arena, module, {indexed_task},
                                                        &wide_index_fixture, &indexed_plan, &indexed_error,
                                                        raster_options));
    vg::hal::CompiledPlan wide_index_compiled;
    assert(device->compile(indexed_plan, &wide_index_compiled, &indexed_error));
    vg::hal::Submission wide_index_submission;
    assert(device->submit(wide_index_compiled, arena, &wide_index_submission, &indexed_error));
    assert(wide_index_submission.result.ok && wide_index_submission.raster_results.size() == 1);

    // Malformed counts are rejected as TriangleList input rather than being
    // truncated to a partial primitive.
    indexed_task.index_count = 4;
    vg::test_support::AssembledPlanFixture malformed_index_fixture;
    assert(!vg::test_support::assemble_single_node_plan(arena, module, {indexed_task},
                                                         &malformed_index_fixture, &indexed_plan, &indexed_error,
                                                         raster_options));
    assert(indexed_error ==
           "raster index facet requires R16Uint/R32Uint and a triangle-list count");
  }
}

void test_user_raster_submit() {
  // --- F3 (ADR-043 Decision #4): a restricted-import "vg.msl.raster/v1"
  // submission -- the resolved Node owns only its user raster contract --
  // must still drive a Raster-kind task exactly like F2's plain path above.
  // This backend never interprets the supplied MSL text (raster_triangles()
  // is completely unchanged), so the pixel output must match F2's fixed
  // C++-shading formula exactly, regardless of what the custom fragment
  // shader source below claims to compute -- a disclosed, intentional
  // limitation (ADR-018: this backend is not a pixel-correctness oracle for
  // user shading logic), asserted here as documented behaviour. compile()
  // must also record the "raster_user_shader" HostAssisted disclosure event
  // (docs/START.md invariant 10: no silent degradation to "verified"). ---
  {
    auto device = vg::reference::make_device_hal();
    assert(device != nullptr);
    const auto& caps = device->capabilities();
    assert(caps.supports(vg::hal::Capability::UserShaderImport));

    vg::core::Arena arena;
    constexpr uint32_t kExtent = 4;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = kExtent, .height = kExtent});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = kExtent, .height = kExtent});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    for (uint32_t y = 0; y < kExtent; ++y)
      for (uint32_t x = 0; x < kExtent; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    std::string error;
    vg::core::FacetRef source_ref, target_ref;
    assert(device->facet_pool().acquire(arena, source, vg::core::FacetKind::Sample, &source_ref, &error));
    assert(device->facet_pool().acquire(arena, target, vg::core::FacetKind::Attachment, &target_ref, &error));

    const auto quad = full_target_quad();
    const uint64_t vertex_bytes = quad.size() * sizeof(vg::reference::RasterVertex);
    auto& vertex_alloc = arena.allocate(vertex_bytes);
    std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
    const vg::core::CanonicalView vertex_view =
        plain_view(vertex_alloc.id, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
    vg::core::FacetRef vertex_ref;
    assert(device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error));

    vg::core::TaskRecord raster_task{};
    raster_task.kind = vg::core::TaskKind::Raster;
    raster_task.raster_facets = {.source = source_ref, .target = target_ref};
    raster_task.vertex_buffer_ref = vertex_ref;
    raster_task.raster_filter = vg::core::FilterMode::Nearest;
    raster_task.raster_wrap = vg::core::WrapMode::Clamp;

    // The imported shader is materialized into a CodeObject and assembled
    // through the same Node/Envelope path as ordinary submissions.
    const vg::ir::UserRasterShaderContract shader{
        "vg.test.raster/v1", "vg_test_vertex", "vg_test_fragment",
        vg::ir::kRasterVertexAbiXyzuvPackedV1,
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct VgRasterVertex { packed_float3 position; float2 uv; };\n"
        "struct VgRasterVaryings { float4 position [[position]]; float2 uv; };\n"
        "vertex VgRasterVaryings vg_test_vertex(device const VgRasterVertex* vertices [[buffer(0)]],\n"
        "                                       uint vid [[vertex_id]]) {\n"
        "  VgRasterVaryings varyings;\n"
        "  varyings.position = float4(vertices[vid].position, 1.0f);\n"
        "  varyings.uv = vertices[vid].uv;\n"
        "  return varyings;\n"
        "}\n"
        "fragment float4 vg_test_fragment(VgRasterVaryings varyings [[stage_in]]) {\n"
        "  return float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
        "}\n"};
    auto stale_shader = shader;
    stale_shader.vertex_abi = "vg.raster.vertex.xyuv-packed/v1";
    vg::test_support::AssembledPlanFixture stale_fixture;
    vg::core::ExecutionPlan stale_plan;
    vg::test_support::AssemblyOptions raster_options;
    raster_options.facet_pool = &device->facet_pool();
    assert(!vg::test_support::assemble_single_user_raster_plan(
        arena, stale_shader, {raster_task}, &stale_fixture, &stale_plan, &error,
        raster_options));
    assert(error.find("vertex_abi") != std::string::npos);
    error.clear();

    vg::test_support::AssembledPlanFixture fixture;
    vg::core::ExecutionPlan plan;
    assert(vg::test_support::assemble_single_user_raster_plan(
        arena, shader, {raster_task}, &fixture, &plan, &error,
        raster_options));

    vg::hal::CompiledPlan compiled;
    assert(device->compile(plan, &compiled, &error));
    assert(compiled.report.supported);
    bool found_user_shader_event = false;
    for (const auto& event : compiled.report.events) {
      if (event.operation == "raster_user_shader" && event.classification == vg::hal::LoweringClass::HostAssisted)
        found_user_shader_event = true;
    }
    assert(found_user_shader_event);

    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    assert(submission.raster_results.size() == 1);
    const auto& task_result = submission.raster_results[0];
    assert(task_result.width == kExtent && task_result.height == kExtent);

    // The reference backend never interprets the supplied MSL text above (it
    // always runs raster_triangles()'s fixed C++ shading), so the pixel
    // output must equal F2's plain fixed-shading oracle exactly -- the same
    // oracle construction as the plain task-graph-driven raster case earlier
    // in this file -- not the solid-red colour the custom fragment shader
    // source claims to produce.
    vg::reference::RasterDesc oracle_desc;
    oracle_desc.filter = raster_task.raster_filter;
    oracle_desc.wrap = raster_task.raster_wrap;
    oracle_desc.attachment =
        vg::hal::f2_default_raster_attachment_config<vg::reference::AttachmentFacetDesc>();
    const auto oracle = vg::reference::raster_triangles(arena, device->facet_pool(), {.source = source_ref, .target = target_ref},
                                                        oracle_desc, quad);
    assert(oracle.ok);
    assert(task_result.resolved_rgba.size() == oracle.resolved_rgba.size());
    for (size_t i = 0; i < oracle.resolved_rgba.size(); ++i)
      assert(exact_match(task_result.resolved_rgba[i], oracle.resolved_rgba[i]));
  }
}

}  // namespace vg::tests::reference
