#include "ioapic.hpp"

#include <stdint.h>

#include "../../serial.hpp"

namespace {
volatile uint8_t* g_base = nullptr;
uint32_t g_gsi_base = 0;
uint32_t g_max_redir = 0;

inline void write_reg(uint32_t reg, uint32_t value) {
    volatile uint32_t* sel = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_base) + 0x00);
    volatile uint32_t* window = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_base) + 0x10);
    *sel = reg;
    *window = value;
}

inline uint32_t read_reg(uint32_t reg) {
    volatile uint32_t* sel = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_base) + 0x00);
    volatile uint32_t* window = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_base) + 0x10);
    *sel = reg;
    return *window;
}

} // namespace

namespace kernel::arch::x86_64::ioapic {

void init(uint32_t phys_addr, uint32_t gsi_base) {
    g_base = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(phys_addr));
    g_gsi_base = gsi_base;

    // read version register (0x01) low byte contains max redirection entry
    const uint32_t ver = read_reg(0x01);
    g_max_redir = ((ver >> 16) & 0xFFu) + 1u;

    kernel::serial::write("[ioapic] init at 0x"); kernel::serial::write_hex(phys_addr);
    kernel::serial::write(" gsi_base="); kernel::serial::write_hex(gsi_base);
    kernel::serial::write(" entries="); kernel::serial::write_hex(g_max_redir);
    kernel::serial::write("\n");

    // Mask all redirection entries initially
    for (uint32_t i = 0; i < g_max_redir; ++i) {
        const uint32_t idx_lo = 0x10 + i * 2;
        const uint32_t idx_hi = 0x10 + i * 2 + 1;

        // set mask bit (bit 16) in low dword
        write_reg(idx_lo, 1u << 16);
        write_reg(idx_hi, 0u);
    }
}

void mask_gsi(uint32_t gsi) {
    if (g_base == nullptr) return;
    if (gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base;
    const uint32_t idx_lo = 0x10 + idx * 2;
    write_reg(idx_lo, 1u << 16);
}

void unmask_gsi(uint32_t gsi) {
    if (g_base == nullptr) return;
    if (gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base;
    const uint32_t idx_lo = 0x10 + idx * 2;
    // clear mask bit
    write_reg(idx_lo, 0u);
}

} // namespace
