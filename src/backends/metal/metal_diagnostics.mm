#include "backends/metal/metal_diagnostics.h"

namespace vg::metal {

hal::LoweringReport make_facet_report() {
  hal::LoweringReport report;
  report.backend = hal::BackendKind::Metal;
  report.supported = true;
  return report;
}

DispatchStats& DispatchStats::operator+=(const DispatchStats& other) {
  cpu_encode_ns += other.cpu_encode_ns;
  cpu_submit_ns += other.cpu_submit_ns;
  encoder_count += other.encoder_count;
  command_buffer_count += other.command_buffer_count;
  barrier_count += other.barrier_count;
  queue_wait_count += other.queue_wait_count;
  return *this;
}

void apply_dispatch_stats(const DispatchStats& stats, hal::Submission* submission) {
  submission->cpu_encode_ns += stats.cpu_encode_ns;
  submission->cpu_submit_ns += stats.cpu_submit_ns;
  submission->report.encoder_count += stats.encoder_count;
  submission->report.command_buffer_count += stats.command_buffer_count;
  submission->report.barrier_count += stats.barrier_count;
  submission->report.queue_wait_count += stats.queue_wait_count;
}

}  // namespace vg::metal
