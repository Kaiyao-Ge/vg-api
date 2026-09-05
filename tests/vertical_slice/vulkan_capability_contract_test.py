#!/usr/bin/env python3
"""Source-contract checks against real Vulkan owners, not SDK/GPU evidence."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
backend = root / "src/backends/vulkan"
owners = {name: (backend / f"vulkan_{name}.cpp").read_text() for name in (
    "device_hal", "resources", "pipelines", "lowering", "commit", "encoding",
    "raster", "plan_raster", "user_raster", "plan_indirect", "diagnostics",
)}
header = (backend / "vulkan_device_hal.h").read_text()
private = (backend / "vulkan_device_internal.h").read_text()


def function(owner: str, signature: str) -> str:
    source = owners[owner]
    assert source.count(signature) == 1, (owner, signature)
    start = source.index(signature)
    # Top-level functions close at column zero; nested blocks stay indented.
    end = source.index("\n}\n", start) + 2
    return source[start:end]


compile_body = function("lowering", "bool DeviceState::compile(")
submit_body = function("commit", "bool DeviceState::submit(")
create_body = function("device_hal", "std::unique_ptr<DeviceHal> DeviceHal::create_impl(")
preflight = compile_body.index("preflight_stage6(plan, capabilities(),")
# C accepts Core-sealed host-assisted discovery and ordinary budget/lease
# plans. Unimplemented paging stays Unsupported; HAL must not derive a second
# access authority. Actual positive/negative behavior belongs to the GPU tests.
assert "!plan.discovery_seeds.empty()" not in compile_body
assert "plan.working_set_budget.has_value() || plan.working_set_lease.has_value()" not in compile_body
for mode in ("SoftwarePaged", "FaultManaged"):
    assert compile_body.index("AccessCertificateMode::" + mode) < preflight
assert "run_discovery_stage(compiled.plan" in submit_body
assert "apply_working_set_budget(compiled.plan" in submit_body
for name, source in owners.items():
    for forbidden in ("discover_reachable(", "build_access_certificate(",
                      "build_discovered_certificate(", "build_complete_lease("):
        assert forbidden not in source, (name, forbidden)

# Optional capability bits must have complete DeviceHAL owners. Raster, GLSL
# user import, and Tier2 are checked below at their feature gates and Stage 6/7
# owners; Tier1 remains absent because this backend exposes Tier2 selection.
assert "Capability::EffectDag);" in create_body
for source in owners.values():
    assert "Capability::IndirectTier1);" not in source
if "Capability::IndirectTier2Select" in create_body:
    assert "submit_plan_tier2_indirect" in submit_body
if "Capability::UserShaderImport" in create_body:
    assert "compile_plan_raster_package" in compile_body
    assert "node_user_raster_package" in owners["plan_raster"]
    assert "ensure_plan_user_raster_pipeline" in owners["plan_raster"]
    assert "submit_plan_raster_step" in submit_body

raster_gate = "const bool raster ="
assert raster_gate in create_body
raster_gate_at = create_body.index(raster_gate)
raster_capability_at = create_body.index("Capability::Raster")
assert raster_gate_at < raster_capability_at
for requirement in (
    "graphics_capable",
    "supports_dynamic_rendering_",
    "sync2",
    "spirv_compiler_available",
    "rgba8_support_.sampled",
    "rgba8_support_.color_attachment",
    "rgba8_support_.transfer_dst",
    "rgba8_support_.transfer_src",
    "d32_support_.depth_stencil_attachment",
    "d32_support_.transfer_dst",
    "d32_support_.transfer_src",
):
    assert requirement in create_body[raster_gate_at:raster_capability_at], requirement

package_at = compile_body.index("compiled->per_node_packages.push_back")
resolved_loop_at = compile_body.index("for (const auto &node : plan.resolved_nodes)")
pipeline_loop_at = compile_body.index("for (size_t index = 0; index < compiled->per_node_packages.size(); ++index)")
assert resolved_loop_at < package_at < pipeline_loop_at
# B uses the same restricted package in lowering and immutable Stage-7
# revalidation. Neither path may quietly substitute the linear package.
for body in (compile_body, submit_body):
    assert "build_pointer_graph_compute_package(*node.module)" in body
    assert "build_linear_compute_package(*node.module)" in body
assert preflight < compile_body.index("build_pointer_graph_compute_package(*node.module)")
assert "pointer_graph ? vg::hal::LoweringClass::CachedObject" in compile_body
assert "load_ref bytes are not checked on GPU" in compile_body
assert compile_body.index('"node_compute_package"', package_at) > package_at
assert "compute_pipeline_cache_" in private
for obsolete in (
    "VkPipeline compute_pipeline_", "VkPipelineLayout pipeline_layout_",
    "VkShaderModule shader_module_", "cached_ir_hash_", "per_node_packages[0]",
    "resolved_nodes[0]", "plan.resolved_nodes.size() > 1",
):
    for name, source in [*owners.items(), ("header", header), ("private", private)]:
        assert obsolete not in source, (name, obsolete)

for obsolete in ("sealed_structural_barriers", "plan.task_order", "validated_effect_graph",
                 "EffectEdgeKind::", "EffectGraph::conflicts", "effect_graph_deterministic_order"):
    for name, source in owners.items():
        assert obsolete not in source, (name, obsolete)
assert "lower_wave_transitions(compiled)" in compile_body
transition_body = function("lowering", "void lower_wave_transitions(")
assert "TransitionLoweringState::Lowered" in transition_body
assert "transition.barrier_count = 1" in transition_body
assert "transition.serialized_fallback = true" in transition_body
assert "compile_plan_raster_package(*this, node" in compile_body
assert "NodePackageKind::Raster" in compile_body

dispatch_body = function("encoding", "bool DeviceState::dispatch_task_graph(")
assert "for (const auto& dispatch : dispatches)" in dispatch_body
assert "dispatch.pipeline->pipeline" in dispatch_body
assert "vkCmdDispatch(command_buffer, dispatch.x, dispatch.y, dispatch.z)" in dispatch_body
assert "vkCmdPipelineBarrier2(command_buffer, &dependency)" in dispatch_body
publication_body = function("encoding", "bool DeviceState::dispatch_task_ring_publication(")
assert "task_ring_pipeline_" in publication_body
assert "vkCmdDispatchIndirect" not in publication_body
assert "compute_pipeline_cache_" not in publication_body
assert "vkCmdPipelineBarrier2(cb, &publication_dependency)" in publication_body

assert "const auto &schedule = compiled.plan.execution_schedule" in submit_body
assert "const auto &component = schedule.components[component_index]" in submit_body
assert "const auto &wave = component.waves[wave_index]" in submit_body
assert "compiled.transition_operations" in submit_body
assert "dispatch.transitions_before" in submit_body
assert submit_body.count("apply_envelope_continuation(compiled.plan") == 1
continuation_at = submit_body.index("apply_envelope_continuation(compiled.plan")
for side_effect in (
    "lifetime_hold.prepare(", "commit_representation_operations(", "lifetime_hold.acquire(",
    "dispatch_task_graph(compute_batch", "submit_plan_raster_step(*this, compiled",
    "std::memcpy(allocation->bytes.data()",
    "dispatch_task_ring_publication(",
):
    assert continuation_at < submit_body.index(side_effect), side_effect
assert "for (uint32_t index : publish_order)" in submit_body
assert "submission->published_tasks.push_back(tasks[index])" in submit_body
assert "submission->report.transition_barrier_count = 0" in submit_body
assert "compute_pipeline_cache_.find(cache_key)" in submit_body
assert "struct ScheduledStep" in submit_body
assert "std::vector<ScheduledStep> steps" in submit_body
assert "dispatch_task_graph(compute_batch" in submit_body
assert "submit_plan_raster_step(*this, compiled" in submit_body
assert "dispatch_task_ring_publication(" in submit_body
schedule_loop_at = submit_body.index("for (const auto &step : steps)")
timeline_signal_at = submit_body.index("submit_timeline_marker(0, signal_value")
assert schedule_loop_at < timeline_signal_at
assert "run_discovery_stage(compiled.plan" in submit_body
for name, source in owners.items():
    assert "compiled.plan.instantiated_effects" not in source, name

# Only the facade delegates; production must not pull in a test implementation.
assert "return state_->compile(plan, compiled, error);" in owners["device_hal"]
assert "return state_->submit(compiled, arena, submission, error);" in owners["device_hal"]
for entry in ("run_sample_facet", "run_storage_facet", "run_raster_facet", "run_pipeline_classification"):
    assert entry not in header, entry
for name, source in owners.items():
    assert '#include "tests/' not in source, name
    assert '#include "vulkan_adapter_harness.h"' not in source, name

# D/F stay in the BUILD_TESTING harness. Physical device feature checks must
# not turn isolated graphics/indirect experiments into production capabilities.
for entry in ("run_gpu_indirect_experiment", "run_gpu_cull_compact_experiment",
              "run_gpu_indexed_address_experiment", "run_gpu_tier2_bucket_experiment",
              "observe_representation_backing"):
    assert entry not in header, entry
    assert all(entry not in source for source in owners.values()), entry
raster_body = function("raster", "bool DeviceState::run_raster_pass(")
assert "supports_dynamic_rendering_" in raster_body
assert "VK_QUEUE_GRAPHICS_BIT" in raster_body
assert "Capability::Raster" not in raster_body
assert "VK_ACCESS_2_HOST_READ_BIT" in raster_body
harness_cmake = (root / "cmake/g4-vulkan-tests.cmake").read_text()
assert "tests/support/vulkan_tier2_harness.cpp" in harness_cmake
assert "SKIP_RETURN_CODE" not in harness_cmake
production_cmake = (root / "cmake/g4-vulkan-sources.cmake").read_text()
assert "tests/" not in production_cmake
assert "src/backends/vulkan/vulkan_plan_raster.cpp" in production_cmake
assert "src/backends/vulkan/vulkan_user_raster.cpp" in production_cmake
assert "src/backends/vulkan/vulkan_plan_indirect.cpp" in production_cmake

print("vulkan capability source contract: ok")
