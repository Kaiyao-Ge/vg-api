#ifndef VG_REFERENCE_EXECUTOR_H_
#define VG_REFERENCE_EXECUTOR_H_

#include "core/core.h"

#include <array>
#include <string>
#include <vector>

namespace vg::reference {

// `timeline` is the device's persistent timeline (owned by the caller across
// submissions). When non-null and `timeline_wait != 0`, execution is refused
// unless the wait point is already satisfied; on success, `timeline_signal`
// (if non-zero) is applied to `*timeline`.
core::ExecutionResult execute(const ir::Module& module, core::Arena& arena,
                              const core::Certificate* certificate = nullptr,
                              core::Timeline* timeline = nullptr,
                              uint64_t timeline_wait = 0, uint64_t timeline_signal = 0);

struct TaskGraphExecutionResult {
  bool ok{};
  std::string message;
  std::vector<core::TaskRecord> published_tasks;
};

// Publishes every task in `task_graph` through a real core::PublicationRing
// (Empty->Writing->Published->Consumed), in an order consistent with its
// Explicit/InferredConflict dependency edges (deterministic: among ready
// tasks, lowest index first). This is the byte-exact oracle Metal/Vulkan
// Task-ring GPU kernels must match.
TaskGraphExecutionResult execute_task_graph(const core::TaskGraph& task_graph);

// TASK-B13 (E009): reference oracle for GPU cull/compact stream compaction.
// Returns every id whose matching instance_visible flag is nonzero, walked
// in ascending instance-index order. This is the CPU-side *set* the GPU
// kernel must reproduce -- but GPU atomic-append slot assignment is ordered
// by thread arrival, not by instance index, so callers must compare the two
// as sets/sorted-multisets, never by position. `ok` is false only on a
// malformed call (mismatched vector sizes), not on any data-dependent case.
struct CullCompactResult {
  bool ok{};
  std::string message;
  std::vector<uint32_t> compact_ids;
};
CullCompactResult cull_compact(const std::vector<uint32_t>& instance_visible,
                               const std::vector<uint32_t>& instance_ids);

// Software nearest/bilinear sampling oracle for SampleFacet correctness checks.
// Reads `view`'s backing allocation directly out of `arena` (no facet/GPU
// resource involved) and samples it at each normalized uv in `uv_coords`,
// using the same half-texel bilinear convention and floor-without-offset
// nearest convention every GPU texture sampler uses (Metal/D3D/Vulkan agree
// on this). RGBA8Unorm channels are returned as float in [0,1]; R32Float's
// single channel is returned as {value, 0, 0, 1}. `ok` is false only for a
// malformed call (unknown allocation, zero-extent view), never on a
// data-dependent sampled value.
struct SampleFacetResult {
  bool ok{};
  std::string message;
  std::vector<std::array<float, 4>> sampled_rgba;
};
SampleFacetResult sample_facet(const core::Arena& arena, const core::CanonicalView& view,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<std::array<float, 2>>& uv_coords);
// Same oracle reached through a capability token: the reference backend
// enforces the pool's kind and staleness rules before it will sample, so a ref
// that a GPU backend must reject cannot quietly produce a reference value to
// compare against.
SampleFacetResult sample_facet(const core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<std::array<float, 2>>& uv_coords);

}  // namespace vg::reference

#endif
