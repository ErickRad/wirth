#pragma once

#include <stdint.h>

namespace kernel {
namespace ahci {

// Probe PCI for AHCI controllers and print information to serial output.
void print_info();

// Initialize AHCI and register block device if present.
bool init();


} // namespace ahci
} // namespace kernel
