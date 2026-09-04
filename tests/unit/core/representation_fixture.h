#pragma once
#include "backends/device_hal.h"
#include "assembled_plan_fixture.h"

namespace vg::tests::core {
vg::ir::Module make_representation_probe_module(const vg::core::Allocation& allocation);
bool assemble_representation_plan(
    vg::core::Arena& arena, const vg::core::Allocation& probe,
    const std::vector<vg::core::RepresentationRequest>& requests,
    const vg::core::FacetPool& pool, vg::test_support::AssembledPlanFixture* fixture,
    vg::core::ExecutionPlan* plan, std::string* error);
}
