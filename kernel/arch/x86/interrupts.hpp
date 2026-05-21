#pragma once

#include <stdint.h>

namespace kernel::arch::x86::interrupts {

void init();
void enable();
void disable();
uint32_t ticks();

// Register a simple IRQ handler for a given IDT vector (e.g. 0x20+irq).
void register_irq_handler(uint8_t vector, void (*handler)());

// Generic IRQ entrypoint called from assembly stubs. Returns new ESP.
extern "C" uint32_t irq_generic_handler(uint32_t current_esp, uint32_t vector);

}  // namespace kernel::arch::x86::interrupts
