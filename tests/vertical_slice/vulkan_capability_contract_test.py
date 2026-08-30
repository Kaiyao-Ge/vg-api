#!/usr/bin/env python3
"""Source-contract review for Vulkan capabilities that need no Vulkan device.

This intentionally checks the compile boundary, not a driver probe: the
repository's normal development hosts may not expose a Vulkan device, while a
capability regression here would otherwise let a plan reach a linear fallback.
"""

from pathlib import Path
import sys


source = (Path(sys.argv[1]) / "src/backends/vulkan/vulkan_device_hal.cpp").read_text()


def must_contain(text: str) -> int:
    position = source.find(text)
    assert position >= 0, text
    return position


compile_start = must_contain("bool DeviceHal::compile(")
preflight = must_contain("preflight_stage6(plan, capabilities(), vg::hal::BackendKind::Vulkan")

for request, operation in (
    ("!plan.effect_dag_passes.empty()", '"effect_dag_lowering"'),
    ("plan.request_tier2_select", '"tier2_select"'),
    ("plan.request_indexed_binding", '"indexed_binding"'),
    ("!plan.discovery_seeds.empty()", '"discovery"'),
    ("plan.working_set_budget.has_value() || plan.working_set_lease.has_value()", '"working_set_sparse"'),
):
    request_at = source.find(request, compile_start)
    operation_at = source.find(operation, request_at)
    assert request_at >= 0, request
    assert operation_at >= 0, operation
    assert compile_start < request_at < preflight, request
    assert request_at < operation_at < preflight, operation

# The capability bits are obligations.  sync2 alone must not advertise a DAG,
# and a standalone facet helper cannot advertise ExecutionPlan Raster support.
assert "Capability::EffectDag);" not in source
assert "Capability::Raster);" not in source

# Indexed plans must be refused before the unconditional linear package path;
# otherwise removing a capability would merely conceal an incorrect fallback.
indexed_at = must_contain("if (plan.request_indexed_binding)")
linear_at = must_contain("build_linear_compute_package(plan.module)")
assert indexed_at < linear_at

print("vulkan capability source contract: ok")
