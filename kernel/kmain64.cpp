#include <stdint.h>

#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "boot/multiboot2.hpp"
#include "serial.hpp"

extern "C" void kernel_main64(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    constexpr uint32_t kExpectedMagic = 0x36D76289;

    kernel::serial::init();
    kernel::serial::write("[kernigham/x86_64] kernel boot\n");
    kernel::serial::write("[kernigham/x86_64] multiboot magic: 0x");
    kernel::serial::write_hex(multiboot_magic);
    kernel::serial::write("\n");
    kernel::serial::write("[kernigham/x86_64] mbi addr: 0x");
    kernel::serial::write_hex(multiboot_info_addr);
    kernel::serial::write("\n");

    if (multiboot_magic != kExpectedMagic) {
        kernel::serial::write("[kernigham/x86_64] invalid multiboot magic\n");
    } else {
        kernel::serial::write("[kernigham/x86_64] bootstrap OK\n");
    }

    kernel::boot::multiboot2::log_memory_map(multiboot_info_addr);
    kernel::arch::x86_64::gdt::init();
    kernel::arch::x86_64::interrupts::init();
    kernel::arch::x86_64::interrupts::enable();
    kernel::serial::write("[kernigham/x86_64] scaffold live\n");

    while (true) {
        asm volatile("hlt");
    }
}
