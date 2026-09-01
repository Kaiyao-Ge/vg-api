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

# Stage 7 consumes the sealed order and records a direct dispatch with each
# Task's own shape and Node pipeline. sync2 barriers come from sealed effects,
# while the Task ring is publication-only and cannot dispatch a Node program.
barrier_helper_start = must_contain("std::vector<uint8_t> sealed_structural_barriers(")
barrier_helper_end = source.find("\n}  // namespace", barrier_helper_start)
barrier_helper = source[barrier_helper_start:barrier_helper_end]
assert "plan.validated_effect_graph.edges()" in barrier_helper
assert "EffectEdgeKind::Explicit" in barrier_helper
assert "EffectEdgeKind::InferredConflict" in barrier_helper
assert "plan.task_order" in barrier_helper
assert "EffectGraph::conflicts" not in barrier_helper

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
assert "for (size_t order_index = 0; order_index < compiled.plan.task_order.size(); ++order_index)" in submit_body
assert "sealed_structural_barriers(plan)" in compile_body
assert "sealed_structural_barriers(compiled.plan)" in submit_body
assert "EffectGraph::conflicts" not in compile_body
assert "EffectGraph::conflicts" not in submit_body
assert "compute_pipeline_cache_.find(cache_key)" in submit_body
assert "dispatch_task_graph(task_dispatches" in submit_body
assert "dispatch_task_ring_publication(ring_buffers" in submit_body
assert "run_discovery_stage(compiled.plan" in source
assert "compiled.plan.instantiated_effects" not in source

print("vulkan capability source contract: ok")
