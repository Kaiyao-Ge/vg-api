#include "backends/vulkan/vulkan_diagnostics.h"

namespace vg::vulkan::detail {

void set_error(std::string* error, const char* message) {
  if (error) *error = message;
}

#if defined(VG_HAS_VULKAN)
vg::hal::LoweringReport make_facet_report() {
  vg::hal::LoweringReport report;
  report.backend = vg::hal::BackendKind::Vulkan;
  report.supported = true;
  return report;
}
#endif

}  // namespace vg::vulkan::detail
