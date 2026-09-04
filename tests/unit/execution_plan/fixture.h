#pragma once

#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"

#include "ir/sha256.h"
#include "vg_scene_root_layout.h"
#include "../../support/assembled_plan_fixture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace vg::tests::execution_plan {

[[noreturn]] void check_failed(const char* expression, const char* file, int line);
vg::core::TaskRecord task(uint32_t node, uint64_t root);
vg::ir::Module canonical_module(uint64_t allocation);
std::shared_ptr<const vg::core::CodeObject> canonical_code_object(uint64_t allocation);
bool assemble_representation_case(
    vg::core::Arena& arena, uint64_t probe_allocation, uint32_t probe_generation,
    const std::vector<vg::core::RepresentationRequest>& requests,
    const vg::core::FacetPool& pool, vg::core::ExecutionPlan* plan, std::string* error);
vg::core::TaskGraph published_graph(std::initializer_list<vg::core::TaskRecord> tasks);
vg::core::CanonicalView rgba_view(const vg::core::Allocation& allocation,
                                  uint32_t width = 1, uint32_t height = 1);

}  // namespace vg::tests::execution_plan

#define CHECK(condition) \
  do { \
    if (!(condition)) check_failed(#condition, __FILE__, __LINE__); \
  } while (false)
