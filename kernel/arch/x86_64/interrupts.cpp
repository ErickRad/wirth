#include "interrupts.hpp"

#include "../../serial.hpp"

namespace {

uint64_t g_ticks = 0;

}  // namespace

namespace kernel::arch::x86_64::interrupts {

struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) IdtPtr {
    uint16_t limit;
    uint64_t base;
};

static IdtEntry g_idt[256];
static IdtPtr g_idt_ptr;

extern "C" void isr_unhandled_stub64();
extern "C" void irq1_stub64();

void set_entry(int index, uintptr_t handler, uint8_t type_attr = 0x8E) {
    g_idt[index].offset_low = static_cast<uint16_t>(handler & 0xFFFF);
    g_idt[index].selector = 0x08; // kernel code segment
    g_idt[index].ist = 0;
    g_idt[index].type_attr = type_attr;
    g_idt[index].offset_mid = static_cast<uint16_t>((handler >> 16) & 0xFFFF);
    g_idt[index].offset_high = static_cast<uint32_t>((handler >> 32) & 0xFFFFFFFF);
    g_idt[index].zero = 0;
}

void load_idt(const IdtPtr& idt) {
    asm volatile("lidt %0" : : "m"(idt));
}

void init() {
    for (int i = 0; i < 256; ++i) {
        set_entry(i, reinterpret_cast<uintptr_t>(isr_unhandled_stub64));
    }

    // PIC remapped IRQ1 -> vector 0x21 (33)
    set_entry(33, reinterpret_cast<uintptr_t>(irq1_stub64));

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = reinterpret_cast<uint64_t>(&g_idt[0]);
    load_idt(g_idt_ptr);
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
