#include "interrupts.hpp"

#include <stdint.h>

#include "pic.hpp"
#include "../../syscall/syscall.hpp"
#include "../../serial.hpp"
#include "../../task/scheduler.hpp"

namespace {

struct [[gnu::packed]] IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
};

struct [[gnu::packed]] IdtPtr {
    uint16_t limit;
    uint32_t base;
};

constexpr uint16_t kKernelCodeSegment = 0x08;
constexpr uint8_t kInterruptGatePresentRing0 = 0x8E;
constexpr uint8_t kTrapGatePresentRing3 = 0xEF;
constexpr int kIdtEntries = 256;
constexpr int kBreakpointVector = 3;
constexpr int kIrq0Vector = 32;
constexpr int kSyscallVector = 128;

IdtEntry g_idt[kIdtEntries] = {};
IdtPtr g_idt_ptr = {};
uint32_t g_ticks = 0;

extern "C" void isr_unhandled_stub();
extern "C" void isr_breakpoint_stub();
extern "C" void irq0_stub();
extern "C" void syscall_stub();
extern "C" void isr_gp_stub();
extern "C" void isr_pf_stub();

// Additional IRQ stubs (defined in assembly)
extern "C" void irq1_stub();
extern "C" void irq2_stub();
extern "C" void irq3_stub();
extern "C" void irq4_stub();
extern "C" void irq5_stub();
extern "C" void irq6_stub();
extern "C" void irq7_stub();
extern "C" void irq8_stub();
extern "C" void irq9_stub();
extern "C" void irq10_stub();
extern "C" void irq11_stub();
extern "C" void irq12_stub();
extern "C" void irq13_stub();
extern "C" void irq14_stub();
extern "C" void irq15_stub();

// IRQ handlers table (indexed by vector)
static void (*g_irq_handlers[256])() = {0};

extern "C" uint32_t irq_generic_handler(uint32_t current_esp, uint32_t vector);

void set_entry(int index, uintptr_t handler, uint8_t type_attr = kInterruptGatePresentRing0) {
    g_idt[index].offset_low = static_cast<uint16_t>(handler & 0xFFFF);
    g_idt[index].selector = kKernelCodeSegment;
    g_idt[index].zero = 0;
    g_idt[index].type_attr = type_attr;
    g_idt[index].offset_high = static_cast<uint16_t>((handler >> 16) & 0xFFFF);
}

void load_idt(const IdtPtr& idt_ptr) {
    asm volatile("lidt %0" : : "m"(idt_ptr));
}

}  // namespace

extern "C" void unhandled_interrupt_handler() {
    uint32_t esp;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    
    kernel::serial::write("[wirth] unhandled interrupt (esp=");
    kernel::serial::write_hex(esp);
    kernel::serial::write(")\n");
    
    while (true) {
        asm volatile("hlt");
    }
}

extern "C" void exception_handler(uint32_t vector, uint32_t error_code) {
    kernel::serial::write("[wirth] exception vec=0x");
    kernel::serial::write_hex(vector);
    kernel::serial::write(" err=0x");
    kernel::serial::write_hex(error_code);
    if (vector == 14) {
        uint32_t cr2 = 0;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        kernel::serial::write(" cr2=0x");
        kernel::serial::write_hex(cr2);
    }
    kernel::serial::write("\n");
    while (true) {
        asm volatile("hlt");
    }
}

extern "C" void breakpoint_handler() {
    kernel::serial::write("[wirth] breakpoint interrupt\n");
}

extern "C" uint32_t irq0_handler(uint32_t current_esp) {
    g_ticks += 1;
    asm volatile("" ::: "memory");

    const uint32_t next_esp = kernel::task::scheduler::on_timer_interrupt(current_esp, g_ticks);
    kernel::arch::x86::pic::send_eoi(0);
    return next_esp;
}

extern "C" uint32_t irq_generic_handler(uint32_t current_esp, uint32_t vector) {
    // If it's the timer IRQ (vector 32), delegate to existing handler
    if (vector == 32u) {
        return irq0_handler(current_esp);
    }

    // Call registered handler if present
    if (vector < 256u && g_irq_handlers[vector] != nullptr) {
        g_irq_handlers[vector]();
        // send EOI for PIC (vector - 0x20)
        const uint8_t irq_line = static_cast<uint8_t>(vector - 0x20u);
        kernel::arch::x86::pic::send_eoi(irq_line);
        return current_esp;
    }

    // Default: send EOI and return
    if (vector >= 0x20u) {
        const uint8_t irq_line = static_cast<uint8_t>(vector - 0x20u);
        kernel::arch::x86::pic::send_eoi(irq_line);
    }
    return current_esp;
}

namespace kernel::arch::x86::interrupts {

void init() {
    for (int i = 0; i < kIdtEntries; ++i) {
        set_entry(i, reinterpret_cast<uintptr_t>(isr_unhandled_stub));
    }

    set_entry(kBreakpointVector, reinterpret_cast<uintptr_t>(isr_breakpoint_stub));

    set_entry(13, reinterpret_cast<uintptr_t>(isr_gp_stub));
    set_entry(14, reinterpret_cast<uintptr_t>(isr_pf_stub));

    set_entry(kIrq0Vector, reinterpret_cast<uintptr_t>(irq0_stub));
    // Wire IRQ1..IRQ15 to generic stubs
    set_entry(kIrq0Vector + 1, reinterpret_cast<uintptr_t>(irq1_stub));
    set_entry(kIrq0Vector + 2, reinterpret_cast<uintptr_t>(irq2_stub));
    set_entry(kIrq0Vector + 3, reinterpret_cast<uintptr_t>(irq3_stub));
    set_entry(kIrq0Vector + 4, reinterpret_cast<uintptr_t>(irq4_stub));
    set_entry(kIrq0Vector + 5, reinterpret_cast<uintptr_t>(irq5_stub));
    set_entry(kIrq0Vector + 6, reinterpret_cast<uintptr_t>(irq6_stub));
    set_entry(kIrq0Vector + 7, reinterpret_cast<uintptr_t>(irq7_stub));
    set_entry(kIrq0Vector + 8, reinterpret_cast<uintptr_t>(irq8_stub));
    set_entry(kIrq0Vector + 9, reinterpret_cast<uintptr_t>(irq9_stub));
    set_entry(kIrq0Vector + 10, reinterpret_cast<uintptr_t>(irq10_stub));
    set_entry(kIrq0Vector + 11, reinterpret_cast<uintptr_t>(irq11_stub));
    set_entry(kIrq0Vector + 12, reinterpret_cast<uintptr_t>(irq12_stub));
    set_entry(kIrq0Vector + 13, reinterpret_cast<uintptr_t>(irq13_stub));
    set_entry(kIrq0Vector + 14, reinterpret_cast<uintptr_t>(irq14_stub));
    set_entry(kIrq0Vector + 15, reinterpret_cast<uintptr_t>(irq15_stub));
    set_entry(
        kSyscallVector,
        reinterpret_cast<uintptr_t>(syscall_stub),
        kTrapGatePresentRing3);

    g_idt_ptr.limit = static_cast<uint16_t>(sizeof(g_idt) - 1);
    g_idt_ptr.base = reinterpret_cast<uint32_t>(&g_idt[0]);
    
    load_idt(g_idt_ptr);
}

void register_irq_handler(uint8_t vector, void (*handler)()) {
    if (vector < 256u) g_irq_handlers[vector] = handler;
}

void enable() {
    asm volatile("sti");
}

void disable() {
    asm volatile("cli");
}

uint32_t ticks() {
    asm volatile("" ::: "memory");
    return g_ticks;
}

}  // namespace kernel::arch::x86::interrupts
