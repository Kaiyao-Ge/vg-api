#ifndef VG_BACKENDS_METAL_DIAGNOSTICS_H_
#define VG_BACKENDS_METAL_DIAGNOSTICS_H_
#include "backends/device_hal.h"
namespace vg::metal {
// Actual physical-command measurements, accumulated at encoding/completion.
// This layer only aggregates them; it never estimates execution from a plan.
struct DispatchStats {
  uint64_t cpu_encode_ns{};
  uint64_t cpu_submit_ns{};
  uint64_t encoder_count{};
  uint64_t command_buffer_count{};
  uint64_t barrier_count{};
  uint64_t queue_wait_count{};

  DispatchStats& operator+=(const DispatchStats& other);
};

hal::LoweringReport make_facet_report();
void apply_dispatch_stats(const DispatchStats& stats, hal::Submission* submission);
}  // namespace vg::metal
#endif
