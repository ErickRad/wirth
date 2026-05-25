#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::lapic {

// Initialize the local APIC at the given physical MMIO base address.
void init(uint32_t phys_addr);

// Send End-Of-Interrupt to the local APIC.
void send_eoi();

// Read/write APIC MMIO register (offset in bytes).
uint32_t read(uint32_t reg);
void write(uint32_t reg, uint32_t value);

}  // namespace kernel::arch::x86_64::lapic
