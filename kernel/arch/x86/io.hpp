#pragma once

#include <stdint.h>

namespace kernel::arch::x86::io {

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void io_wait() {
    outb(0x80, 0);
}

}  // namespace kernel::arch::x86::io
