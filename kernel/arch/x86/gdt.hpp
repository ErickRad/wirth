#pragma once

#include <stdint.h>

namespace kernel::arch::x86::gdt {

void init();
void set_kernel_stack(uint32_t stack_top);

}  // namespace kernel::arch::x86::gdt
