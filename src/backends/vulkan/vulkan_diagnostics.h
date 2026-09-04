#ifndef VG_BACKENDS_VULKAN_DIAGNOSTICS_H_
#define VG_BACKENDS_VULKAN_DIAGNOSTICS_H_

#include "backends/device_hal.h"

namespace vg::vulkan {

namespace detail {
void set_error(std::string* error, const char* message);
vg::hal::LoweringReport make_facet_report();
}
}  // namespace vg::vulkan

#endif
