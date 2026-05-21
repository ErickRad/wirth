#include "gdt.hpp"

#include "../../serial.hpp"

namespace kernel::arch::x86_64::gdt {

void init() {
}

void set_kernel_stack(uint64_t stack_top) {
    kernel::serial::write("[wirth] set_kernel_stack=0x");
    kernel::serial::write_hex64(stack_top);
    kernel::serial::write("\n");
}

}  // namespace kernel::arch::x86_64::gdt
