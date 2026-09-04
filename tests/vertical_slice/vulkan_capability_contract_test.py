#!/usr/bin/env python3
"""Source-contract checks against real Vulkan owners, not SDK/GPU evidence."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
backend = root / "src/backends/vulkan"
owners = {name: (backend / f"vulkan_{name}.cpp").read_text() for name in (
    "device_hal", "resources", "pipelines", "lowering", "commit", "encoding",
    "raster", "diagnostics",
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
preflight = compile_body.index("preflight_stage6(plan, capabilities(), vg::hal::BackendKind::Vulkan")
for request, operation in (
    ("!plan.discovery_seeds.empty()", '"discovery"'),
    ("plan.working_set_budget.has_value() || plan.working_set_lease.has_value()", '"working_set_sparse"'),
):
    request_at = compile_body.index(request)
    operation_at = compile_body.index(operation, request_at)
    assert request_at < operation_at < preflight, operation

# Physical helpers cannot advertise production capabilities.
assert "Capability::EffectDag);" in create_body
for source in owners.values():
    assert "Capability::IndirectTier1);" not in source
    assert "Capability::Raster);" not in source

package_at = compile_body.index("compiled->per_node_packages.push_back")
resolved_loop_at = compile_body.index("for (const auto& node : plan.resolved_nodes)")
pipeline_loop_at = compile_body.index("for (size_t index = 0; index < compiled->per_node_packages.size(); ++index)")
assert resolved_loop_at < package_at < pipeline_loop_at
assert preflight < compile_body.index("Vulkan pointer-graph Node lowering is Unsupported")
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
assert '"raster_task", "Vulkan Unsupported: NodeRef{"' in compile_body
assert '"} domain=Raster; complete plan rejected before Commit"' in compile_body
assert "std::to_string(task.node_generation)" in compile_body
assert compile_body.index("task.kind == vg::core::TaskKind::Raster") < preflight

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

assert "const auto& schedule = compiled.plan.execution_schedule" in submit_body
assert "const auto& component = schedule.components[component_index]" in submit_body
assert "const auto& wave = component.waves[wave_index]" in submit_body
assert "compiled.transition_operations" in submit_body
assert "dispatch.transitions_before" in submit_body
assert submit_body.count("apply_envelope_continuation(compiled.plan") == 1
continuation_at = submit_body.index("apply_envelope_continuation(compiled.plan")
for side_effect in (
    "lifetime_hold.prepare(", "commit_representation_operations(", "lifetime_hold.acquire(",
    "dispatch_task_graph(task_dispatches", "std::memcpy(allocation->bytes.data()",
    "dispatch_task_ring_publication(ring_buffers",
):
    assert continuation_at < submit_body.index(side_effect), side_effect
assert "for (uint32_t index : publish_order)" in submit_body
assert "submission->published_tasks.push_back(tasks[index])" in submit_body
assert "submission->report.transition_barrier_count = 0" in submit_body
assert "compute_pipeline_cache_.find(cache_key)" in submit_body
assert "dispatch_task_graph(task_dispatches" in submit_body
assert "dispatch_task_ring_publication(ring_buffers" in submit_body
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

print("vulkan capability source contract: ok")
