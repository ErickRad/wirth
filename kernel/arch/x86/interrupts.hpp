#pragma once

#include <stdint.h>

namespace kernel::arch::x86::interrupts {

void init();
void enable();
void disable();
uint32_t ticks();

}  // namespace kernel::arch::x86::interrupts
