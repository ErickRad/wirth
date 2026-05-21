#pragma once

#include <stdint.h>

namespace kernel {
namespace nvme {

// Probe NVMe controllers and initialize minimal support.
bool init();

} // namespace nvme
} // namespace kernel
