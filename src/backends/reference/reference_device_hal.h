#ifndef VG_BACKENDS_REFERENCE_DEVICE_HAL_H_
#define VG_BACKENDS_REFERENCE_DEVICE_HAL_H_

#include "backends/device_hal.h"

namespace vg::reference {
std::unique_ptr<hal::DeviceHal> make_device_hal();
}

#endif
