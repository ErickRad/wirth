#include "gdt.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

struct [[gnu::packed]] GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
};

struct [[gnu::packed]] GdtPtr {
    uint16_t limit;
    uint32_t base;
};

struct [[gnu::packed]] TssEntry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
};

constexpr int kGdtEntries = 6;
constexpr uint16_t kKernelDataSelector = 0x10;

GdtEntry g_gdt[kGdtEntries] = {};
GdtPtr g_gdt_ptr = {};
TssEntry g_tss = {};

extern "C" void gdt_flush(uint32_t gdt_ptr_addr);
extern "C" void tss_flush();

void set_gdt_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    g_gdt[index].base_low = static_cast<uint16_t>(base & 0xFFFF);
    g_gdt[index].base_middle = static_cast<uint8_t>((base >> 16) & 0xFF);
    g_gdt[index].base_high = static_cast<uint8_t>((base >> 24) & 0xFF);

    g_gdt[index].limit_low = static_cast<uint16_t>(limit & 0xFFFF);
    g_gdt[index].granularity = static_cast<uint8_t>((limit >> 16) & 0x0F);
    g_gdt[index].granularity |= static_cast<uint8_t>(granularity & 0xF0);
    g_gdt[index].access = access;
}

void clear_tss() {
    uint8_t* raw = reinterpret_cast<uint8_t*>(&g_tss);
    for (size_t i = 0; i < sizeof(TssEntry); ++i) {
        raw[i] = 0;
    }
}

void write_tss(int index, uint16_t ss0, uint32_t esp0) {
    const uint32_t base = reinterpret_cast<uint32_t>(&g_tss);
    const uint32_t limit = sizeof(TssEntry) - 1;

    set_gdt_entry(index, base, limit, 0x89, 0x00);
    clear_tss();
    g_tss.ss0 = ss0;
    g_tss.esp0 = esp0;
    g_tss.cs = 0x0B;
    g_tss.ss = 0x13;
    g_tss.ds = 0x13;
    g_tss.es = 0x13;
    g_tss.fs = 0x13;
    g_tss.gs = 0x13;
    g_tss.iomap_base = sizeof(TssEntry);
}

}  // namespace

namespace kernel::arch::x86::gdt {

void init() {
    g_gdt_ptr.limit = static_cast<uint16_t>(sizeof(GdtEntry) * kGdtEntries - 1);
    g_gdt_ptr.base = reinterpret_cast<uint32_t>(&g_gdt[0]);

    set_gdt_entry(0, 0, 0, 0, 0);
    set_gdt_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    set_gdt_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    set_gdt_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    set_gdt_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    write_tss(5, kKernelDataSelector, 0);

    gdt_flush(reinterpret_cast<uint32_t>(&g_gdt_ptr));
    tss_flush();
}

void set_kernel_stack(uint32_t stack_top) {
    g_tss.esp0 = stack_top;
}

}  // namespace kernel::arch::x86::gdt
