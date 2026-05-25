#include "../../serial.hpp"
#include "../../arch/x86/io.hpp"
#include "lapic.hpp"
#include "../../input/keyboard.hpp"

extern "C" void irq1_handler64() {
    // Read scancode from PS/2 controller
    const uint8_t sc = kernel::arch::x86::io::inb(0x60);
    if ((sc & 0x80u) == 0) {
        kernel::input::keyboard::enqueue_from_isr(sc);
    }

    // Send EOI via local APIC
    kernel::arch::x86_64::lapic::send_eoi();
}

extern "C" void isr_unhandled_handler64() {
    kernel::serial::write("[wirth] unhandled interrupt 64\n");
    while (true) asm volatile("hlt");
}
