#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::interrupts {

void init();
void enable();
void disable();
uint64_t ticks();

}  // namespace kernel::arch::x86_64::interrupts
