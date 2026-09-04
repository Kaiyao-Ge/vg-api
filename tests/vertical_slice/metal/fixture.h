#pragma once
#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "backends/metal/metal_physical_types.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "capture/capture.h"
#include "compiler/compiler.h"
#include "ir/ir.h"
#include "assembled_plan_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace vg::tests::metal {
using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;
inline constexpr float kNearestTol = 1.0f / 255.0f + 1e-4f;
struct Extent2 {
  uint32_t width{};
  uint32_t height{};
};

bool assemble_compute_plan(vg::core::Arena& arena, vg::ir::Module module,
                           std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
                           std::string* error, const vg::test_support::AssemblyOptions& options = {});
bool assemble_user_raster_plan(vg::core::Arena& arena, const vg::ir::UserRasterShaderContract& shader,
                               std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
                               std::string* error, const vg::test_support::AssemblyOptions& options = {});
TaskRecord probe_task(const vg::ir::Module& module);
vg::ir::Module make_probe_module(vg::core::Arena& arena);
bool same_task(const TaskRecord& a, const TaskRecord& b);
bool channels_close(const std::array<float, 4>& got, const std::array<float, 4>& want, float tol,
                    const char* label, const char* what);
void fill_subresource(vg::core::Allocation& allocation, const vg::core::CanonicalView& view,
                      uint32_t layer, uint32_t level, const std::array<uint8_t, 4>& rgba);
std::vector<vg::reference::RasterVertex> to_reference_vertices(
    const std::vector<vg::metal::RasterVertex>& vertices);
vg::reference::RasterDesc to_reference_desc(const vg::metal::RasterDesc& desc);
vg::core::ConsumeProof complete_consume_proof();
vg::core::CanonicalView make_rgba8_view(const vg::core::Allocation& allocation, Extent2 extent);
vg::core::CanonicalView make_depth32_view(const vg::core::Allocation& allocation, Extent2 extent);
std::vector<vg::metal::RasterVertex> metal_fullscreen_quad();
vg::ir::Module make_epoch_probe_module(const vg::core::Allocation& allocation, uint32_t epoch);
vg::core::Allocation& prepare_consume_image(vg::core::Arena& arena, vg::core::CanonicalView* view);
bool run_consume_fault_during();
bool check_post_consume_sample(vg::metal::DeviceHal* metal_device, vg::core::Arena& arena,
                               const vg::hal::Submission& submission,
                               const std::vector<std::array<float, 2>>& uvs,
                               const vg::reference::SampleFacetResult& expected,
                               std::string& error);
bool check_compiled_plan_tampering(vg::metal::DeviceHal* metal_device, vg::core::Arena& arena,
                                   const vg::core::Allocation& target,
                                   const vg::hal::CompiledPlan& compiled, std::string& error);
}
