#include <stdint.h>

#include "boot/multiboot2.hpp"
#include "boot/acpi.hpp"
#include "arch/x86/gdt.hpp"
#include "arch/x86/interrupts.hpp"
#include "arch/x86/pic.hpp"
#include "arch/x86/pit.hpp"
#include "fs/ramfs.hpp"
#include "fs/vfs.hpp"
#include "storage.hpp"
#include "block.hpp"
#include "block_partition.hpp"
#include "loader/userland.hpp"
#include "mm/heap.hpp"
#include "mm/pmm.hpp"
#include "mm/vmm.hpp"
#include "syscall/syscall.hpp"
#include "task/scheduler.hpp"
#include "serial.hpp"
#include "shell.hpp"
#include "xhci.hpp"
#include "usb_mass_storage.hpp"

extern "C" uint8_t __kernel_start;
extern "C" uint8_t __kernel_end;

struct BootMarker {
    uint32_t id;
    uint32_t flags;
};

uint32_t syscall0(uint32_t number) {
    uint32_t ret = 0;
    asm volatile("int $0x80" : "=a"(ret) : "a"(number) : "memory");
    return ret;
}

void worker_task_a() {
    while (true) {
        asm volatile("hlt");
    }
}

void worker_task_b() {
    while (true) {
        asm volatile("hlt");
    }
}

void worker_task_c() {
    while (true) {
        asm volatile("hlt");
    }
}

extern "C" void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    constexpr uint32_t kExpectedMagic = 0x36D76289;

    kernel::serial::init();
    kernel::serial::write("[wirth] kernel boot\n");
    kernel::serial::write("[wirth] multiboot magic: 0x");
    kernel::serial::write_hex(multiboot_magic);
    kernel::serial::write("\n");
    kernel::serial::write("[wirth] mbi addr: 0x");
    kernel::serial::write_hex(multiboot_info_addr);
    kernel::serial::write("\n");

    if (multiboot_magic != kExpectedMagic) {
        kernel::serial::write("[wirth] invalid multiboot magic\n");
    } else {
        kernel::serial::write("[wirth] bootstrap OK\n");
    }
    kernel::boot::multiboot2::log_memory_map(multiboot_info_addr);
    kernel::boot::acpi::log(multiboot_info_addr);
    kernel::mm::pmm::init(
        multiboot_info_addr,
        reinterpret_cast<uint32_t>(&__kernel_start),
        reinterpret_cast<uint32_t>(&__kernel_end));

    kernel::mm::vmm::init();
    kernel::mm::vmm::enable_paging();
    kernel::serial::write("[wirth] paging enabled\n");

    const uint32_t mapped_test_virt = 0xC0400000u;
    const uint32_t mapped_test_phys = kernel::mm::pmm::alloc_frame();

    kernel::mm::heap::init(0xD0000000u, 0xD1000000u);
    void* const heap_ptr = kernel::mm::heap::alloc(128, 16);
    
    kernel::fs::RamFs ramfs;

    kernel::storage_init();

    kernel::fs::g_fs = &ramfs;

    if (kernel::fs::g_fs->init()) {
        kernel::serial::write("[wirth] ramfs ready\n");
    } else {
        kernel::serial::write("[wirth] ramfs init failed\n");
    }

    // restore any packages persisted on disk into ramfs
    kernel::storage_restore_packages();

    kernel::serial::write("[wirth/x86] xhci probe\n");
    const bool xhci_ready = kernel::xhci::init();
    kernel::serial::write(xhci_ready ? "[wirth/x86] xhci ready\n" : "[wirth/x86] xhci unavailable\n");

    kernel::serial::write("[wirth/x86] usbms probe\n");
    const bool usbms_ready = kernel::usbms::init();
    kernel::serial::write(usbms_ready ? "[wirth/x86] usbms ready\n" : "[wirth/x86] usbms unavailable\n");

    kernel::block_visit_devices([](kernel::BlockDevice* dev, void*) {
        if (dev != nullptr) {
            kernel::block_partition::scan_mbr_partitions(dev);
        }
    }, nullptr);

    kernel::arch::x86::gdt::init();
    uint32_t current_stack = 0;

    asm volatile("mov %%esp, %0" : "=r"(current_stack));
    kernel::arch::x86::gdt::set_kernel_stack(current_stack);
    kernel::serial::write("[wirth] gdt+tss loaded\n");
    kernel::task::scheduler::init(current_stack);
    kernel::serial::write("[wirth] scheduler initialized\n");

    const int process_a = kernel::task::scheduler::create_process_task("svc-a", worker_task_a, 16 * 1024);
    const int process_b = kernel::task::scheduler::create_process_task("svc-b", worker_task_b, 16 * 1024);
    const int process_c = kernel::task::scheduler::create_process_task("svc-c", worker_task_c, 16 * 1024);

    kernel::serial::write("[wirth] processes created: A=0x");

    kernel::serial::write_hex(static_cast<uint32_t>(process_a));
    kernel::serial::write(" B=0x");

    kernel::serial::write_hex(static_cast<uint32_t>(process_b));
    kernel::serial::write(" C=0x");

    kernel::serial::write_hex(static_cast<uint32_t>(process_c));
    kernel::serial::write("\n");

    kernel::arch::x86::interrupts::init();
    kernel::serial::write("[wirth] idt loaded\n");

    kernel::arch::x86::pic::remap(0x20, 0x28);
    kernel::arch::x86::pic::clear_irq_mask(0);

    kernel::arch::x86::pit::init(100);
    kernel::serial::write("[wirth] pit configured (100hz)\n");

    kernel::arch::x86::interrupts::enable();
    kernel::serial::write("[wirth] interrupts enabled\n");
    kernel::serial::write("[wirth] root uid=0x");

    kernel::serial::write_hex(syscall0(kernel::syscall::kGetUid));
    kernel::serial::write(" gid=0x");

    kernel::serial::write_hex(syscall0(kernel::syscall::kGetGid));
    kernel::serial::write("\n");

    const char file_path[] = "/test.txt";
    char file_buf[64] = {};
    uint32_t file_fd = 0;

    asm volatile("int $0x80"
                 : "=a"(file_fd)
                 : "a"(kernel::syscall::kOpen), "b"(file_path), "c"(kernel::fs::kOpenRead)
                 : "memory");

    if (file_fd != 0xFFFFFFFFu) {
        uint32_t read_n = 0;

        asm volatile("int $0x80"
                     : "=a"(read_n)
                     : "a"(kernel::syscall::kRead), "b"(file_fd), "c"(file_buf),
                       "d"(sizeof(file_buf) - 1)
                     : "memory");

        if (read_n != 0xFFFFFFFFu && read_n < sizeof(file_buf)) {
            file_buf[read_n] = '\0';

            kernel::serial::write(file_buf);
            kernel::serial::write("\n");

        } else {
            kernel::serial::write("[wirth] ramfs read failed\n");
        }

        asm volatile("int $0x80" : : "a"(kernel::syscall::kClose), "b"(file_fd) : "memory");

    } else {
        kernel::serial::write("[wirth] ramfs open failed\n");
    }

    const char passwd_path[] = "/etc/passwd";
    uint32_t passwd_fd = 0;
    asm volatile("int $0x80"
                 : "=a"(passwd_fd)
                 : "a"(kernel::syscall::kOpen), "b"(passwd_path), "c"(kernel::fs::kOpenRead)
                 : "memory");

    if (passwd_fd != 0xFFFFFFFFu) {
        char passwd_buf[96] = {};
        uint32_t passwd_n = 0;

        asm volatile("int $0x80"
                     : "=a"(passwd_n)
                     : "a"(kernel::syscall::kRead), "b"(passwd_fd), "c"(passwd_buf),
                       "d"(sizeof(passwd_buf) - 1)
                     : "memory");
        
        if (passwd_n != 0xFFFFFFFFu && passwd_n < sizeof(passwd_buf)) {                
            passwd_buf[passwd_n] = '\0';

        }

        asm volatile("int $0x80" : : "a"(kernel::syscall::kClose), "b"(passwd_fd) : "memory");
    }

    const char dir_path[] = "/bin";
    asm volatile("int $0x80" : : "a"(kernel::syscall::kMd), "b"(dir_path) : "memory");

    const char nested_path[] = "/bin/note.txt";
    uint32_t nested_fd = 0;

    asm volatile("int $0x80"
                 : "=a"(nested_fd)
                 : "a"(kernel::syscall::kOpen), "b"(nested_path),
                   "c"(kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate)
                 : "memory");

    if (nested_fd != 0xFFFFFFFFu) {
        const char nested_data[] = "Nested file OK\n";

        asm volatile("int $0x80"
                     :
                     : "a"(kernel::syscall::kWrite), "b"(nested_fd), "c"(nested_data),
                       "d"(sizeof(nested_data) - 1)
                     : "memory");

        asm volatile("int $0x80" : : "a"(kernel::syscall::kClose), "b"(nested_fd) : "memory");
    }

    asm volatile("int $0x80"
                 : "=a"(nested_fd)
                 : "a"(kernel::syscall::kOpen), "b"(nested_path), "c"(kernel::fs::kOpenRead)
                 : "memory");

    if (nested_fd != 0xFFFFFFFFu) {
        char nested_buf[64] = {};
        uint32_t nested_n = 0;

        asm volatile("int $0x80"
                     : "=a"(nested_n)
                     : "a"(kernel::syscall::kRead), "b"(nested_fd), "c"(nested_buf),
                       "d"(sizeof(nested_buf) - 1)
                     : "memory");

        if (nested_n != 0xFFFFFFFFu && nested_n < sizeof(nested_buf)) {
            nested_buf[nested_n] = '\0';
        }

        asm volatile("int $0x80" : : "a"(kernel::syscall::kClose), "b"(nested_fd) : "memory");
    }

    kernel::shell::run();
    
    while (true) {
        asm volatile("hlt");
    }
}
