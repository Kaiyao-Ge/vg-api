#!/usr/bin/env python3
"""Source-contract review for Vulkan capabilities that need no Vulkan device.

This intentionally checks the compile boundary, not a driver probe: the
repository's normal development hosts may not expose a Vulkan device, while a
capability regression here would otherwise let a plan reach a linear fallback.
"""

from pathlib import Path
import sys


root = Path(sys.argv[1])
source = (root / "src/backends/vulkan/vulkan_device_hal.cpp").read_text()
header = (root / "src/backends/vulkan/vulkan_device_hal.h").read_text()


def must_contain(text: str) -> int:
    position = source.find(text)
    assert position >= 0, text
    return position


compile_start = must_contain("bool DeviceHal::compile(")
submit_start = must_contain("bool DeviceHal::submit(")
preflight = must_contain("preflight_stage6(plan, capabilities(), vg::hal::BackendKind::Vulkan")
compile_body = source[compile_start:submit_start]

for request, operation in (
    ("!plan.discovery_seeds.empty()", '"discovery"'),
    ("plan.working_set_budget.has_value() || plan.working_set_lease.has_value()", '"working_set_sparse"'),
):
    request_at = source.find(request, compile_start)
    operation_at = source.find(operation, request_at)
    assert request_at >= 0, request
    assert operation_at >= 0, operation
    assert compile_start < request_at < preflight, request
    assert request_at < operation_at < preflight, operation

# The capability bits are obligations. EffectDag is advertised only beside
# the implemented sync2 TaskPublication path; the removed Task-ring indirect
# execution must not leave an IndirectTier1 promise behind. A standalone facet
# helper still cannot advertise ExecutionPlan Raster support.
assert "Capability::EffectDag);" in source
assert "Capability::IndirectTier1);" not in source
assert "Capability::Raster);" not in source

# Stage 6 walks every immutable Node, builds one full-NodeRef package, and
# ensures one backend-owned pipeline without a singleton eviction cache.
package_at = compile_body.find("compiled->per_node_packages.push_back")
resolved_loop_at = compile_body.find("for (const auto& node : plan.resolved_nodes)")
pipeline_loop_at = compile_body.find("for (size_t index = 0; index < compiled->per_node_packages.size(); ++index)")
assert 0 <= resolved_loop_at < package_at < pipeline_loop_at
pointer_reject_at = must_contain("Vulkan pointer-graph Node lowering is Unsupported")
assert preflight < pointer_reject_at < submit_start
node_package_event_at = compile_body.find('"node_compute_package"', package_at)
assert node_package_event_at > package_at
assert "compute_pipeline_cache_" in header
for obsolete in (
    "VkPipeline compute_pipeline_",
    "VkPipelineLayout pipeline_layout_",
    "VkShaderModule shader_module_",
    "cached_ir_hash_",
    "per_node_packages[0]",
    "resolved_nodes[0]",
    "plan.resolved_nodes.size() > 1",
):
    assert obsolete not in source + header, obsolete

# Stage 7 consumes the sealed schedule and records a direct dispatch with each
# Task's own shape and Node pipeline. sync2 barriers come from wave operations,
# while the Task ring is publication-only and cannot dispatch a Node program.
for obsolete in ("sealed_structural_barriers", "plan.task_order", "validated_effect_graph",
                 "EffectEdgeKind::", "EffectGraph::conflicts", "effect_graph_deterministic_order"):
    assert obsolete not in source, obsolete
assert "lower_wave_transitions(compiled)" in compile_body
assert "TransitionLoweringState::Lowered" in source
assert "transition.barrier_count = 1" in source
assert "transition.serialized_fallback = true" in source
assert '"raster_task", "Vulkan Unsupported: NodeRef{"' in compile_body
assert '"} domain=Raster; complete plan rejected before Commit"' in compile_body
assert "std::to_string(task.node_generation)" in compile_body
assert compile_body.find("task.kind == vg::core::TaskKind::Raster") < compile_body.find("preflight_stage6")

dispatch_start = must_contain("bool DeviceHal::dispatch_task_graph(")
timeline_start = must_contain("bool DeviceHal::ensure_timeline_semaphore(")
dispatch_body = source[dispatch_start:timeline_start]
assert "for (const auto& dispatch : dispatches)" in dispatch_body
assert "dispatch.pipeline->pipeline" in dispatch_body
assert "vkCmdDispatch(command_buffer, dispatch.x, dispatch.y, dispatch.z)" in dispatch_body
assert "vkCmdPipelineBarrier2(command_buffer, &dependency)" in dispatch_body

publication_start = must_contain("bool DeviceHal::dispatch_task_ring_publication(")
publication_end = source.find("\nnamespace {", publication_start)
publication_body = source[publication_start:publication_end]
assert "task_ring_pipeline_" in publication_body
assert "vkCmdDispatchIndirect" not in publication_body
assert "compute_pipeline_cache_" not in publication_body
assert "vkCmdPipelineBarrier2(cb, &publication_dependency)" in publication_body

submit_body = source[submit_start:source.find("// --- Phase C facet entry points", submit_start)]
assert "const auto& schedule = compiled.plan.execution_schedule" in submit_body
assert "const auto& component = schedule.components[component_index]" in submit_body
assert "const auto& wave = component.waves[wave_index]" in submit_body
assert "compiled.transition_operations" in submit_body
assert "dispatch.transitions_before" in source
assert "apply_envelope_continuation(compiled.plan" in submit_body
assert submit_body.count("apply_envelope_continuation(compiled.plan") == 1
continuation_at = submit_body.index("apply_envelope_continuation(compiled.plan")
for side_effect in (
    "lifetime_hold.prepare(",
    "commit_representation_operations(",
    "lifetime_hold.acquire(",
    "dispatch_task_graph(task_dispatches",
    "std::memcpy(allocation->bytes.data()",
    "dispatch_task_ring_publication(ring_buffers",
):
    assert continuation_at < submit_body.index(side_effect), side_effect
assert "for (uint32_t index : publish_order)" in submit_body
assert "submission->published_tasks.push_back(tasks[index])" in submit_body
assert "submission->report.transition_barrier_count = 0" in submit_body
assert "EffectGraph::conflicts" not in compile_body
assert "EffectGraph::conflicts" not in submit_body
assert "compute_pipeline_cache_.find(cache_key)" in submit_body
assert "dispatch_task_graph(task_dispatches" in submit_body
assert "dispatch_task_ring_publication(ring_buffers" in submit_body
assert "run_discovery_stage(compiled.plan" in source
assert "compiled.plan.instantiated_effects" not in source

print("vulkan capability source contract: ok")
