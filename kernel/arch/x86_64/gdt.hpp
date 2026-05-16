#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::gdt {

void init();
void set_kernel_stack(uint64_t stack_top);

}  // namespace kernel::arch::x86_64::gdt
