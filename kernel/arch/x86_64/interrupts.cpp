#include "interrupts.hpp"

#include "../../serial.hpp"

namespace {

uint64_t g_ticks = 0;

}  // namespace

namespace kernel::arch::x86_64::interrupts {

void init() {
    kernel::serial::write("[kernigham/x86_64] interrupts scaffold init\n");
}

void enable() {
    asm volatile("sti");
}

void disable() {
    asm volatile("cli");
}

uint64_t ticks() {
    asm volatile("" ::: "memory");
    return g_ticks;
}

}  // namespace kernel::arch::x86_64::interrupts
