#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::ioapic {

// Initialize an IOAPIC at the given physical MMIO base and GSI base index.
void init(uint32_t phys_addr, uint32_t gsi_base);

// Mask (disable) the given GSI line.
void mask_gsi(uint32_t gsi);

// Unmask (enable) the given GSI line.
void unmask_gsi(uint32_t gsi);

} // namespace kernel::arch::x86_64::ioapic
