#include "lapic.hpp"

#include <stdint.h>

#include "../../serial.hpp"

namespace {
volatile uint8_t* g_base = nullptr;

inline void mmio_write(uint32_t reg, uint32_t val) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_base) + reg);
    *ptr = val;
}

inline uint32_t mmio_read(uint32_t reg) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_base) + reg);
    return *ptr;
}

}  // namespace

namespace kernel::arch::x86_64::lapic {

void init(uint32_t phys_addr) {
    g_base = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(phys_addr));

    // Enable the local APIC by setting bit 8 of the Spurious Interrupt Vector Register (offset 0xF0)
    const uint32_t svr = mmio_read(0xF0);
    mmio_write(0xF0, svr | 0x100u);

    kernel::serial::write("[lapic] initialized\n");
}

void send_eoi() {
    mmio_write(0xB0, 0u);
}

uint32_t read(uint32_t reg) {
    return mmio_read(reg);
}

void write(uint32_t reg, uint32_t value) {
    mmio_write(reg, value);
}

}  // namespace kernel::arch::x86_64::lapic
