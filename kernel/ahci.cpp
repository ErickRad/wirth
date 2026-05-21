#include "ahci.hpp"

#include "block.hpp"

#include "pci.hpp"
#include "serial.hpp"
#include "drivers/ide.hpp"

#include "mm/heap.hpp"
#include "mm/vmm.hpp"
#include "task/scheduler.hpp"
#ifndef __x86_64__
#include "arch/x86/interrupts.hpp"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stddef.h>
extern "C" void* memset(void* s, int c, size_t n);

namespace {

#ifndef __x86_64__
[[maybe_unused]] static void* map_physical_region(uint32_t phys, uint32_t size) {
    const uint32_t page = 4096u;
    const uint32_t pages = (size + page - 1u) / page;

    void* virt = kernel::mm::heap::alloc(pages * page, page);

    if (virt == nullptr) return nullptr;

    uint64_t v = reinterpret_cast<uintptr_t>(virt);
    uint32_t p = phys & 0xFFFFF000u;

    for (uint32_t i = 0; i < pages; ++i) {
        if (!kernel::mm::vmm::map_page(v + i * page, p + i * page, true, false)) {
            return nullptr;
        }
    }

    return virt;
}
#else
[[maybe_unused]] static void* map_physical_region(uint32_t /*phys*/, uint32_t /*size*/) {
    return nullptr;
}
#endif

} // namespace

namespace kernel {
namespace ahci {
struct PortState {
    volatile uint32_t* regs;
    void* clb;
    void* fis;
    void* cmd_tables;
    bool present;
};

static volatile uint32_t* g_hba_regs = nullptr;
static PortState g_ports[32];

#ifndef __x86_64__
static void stop_port(uint32_t port);
static void start_port(uint32_t port);
static bool ahci_read_sectors(uint64_t lba, uint8_t* buf, uint32_t count);
static bool ahci_write_sectors(uint64_t lba, const uint8_t* buf, uint32_t count);
#else
static bool ahci_read_sectors(uint64_t lba, uint8_t* buf, uint32_t count);
#endif

void print_info() {
    kernel::pci::Device devs[64];
    uint32_t found = 0;

    if (!kernel::pci::scan_devices(devs, 64, &found) || found == 0) {
        kernel::serial::write("ahci: no PCI devices found\n");
        return;
    }

    for (uint32_t i = 0; i < found; ++i) {
        const kernel::pci::Device& d = devs[i];

        if (d.class_code != 0x01u || d.subclass != 0x06u) continue;

        kernel::serial::write("AHCI controller at ");
        kernel::serial::write_hex(d.bus);
        kernel::serial::write(":");

        kernel::serial::write_hex(d.slot);
        kernel::serial::write(":");

        kernel::serial::write_hex(d.func);
        kernel::serial::write(" vendor=0x"); kernel::serial::write_hex(d.vendor_id);
        kernel::serial::write(" device=0x"); kernel::serial::write_hex(d.device_id);

        kernel::serial::write("\n");

        uint32_t mmio = 0;

        for (int b = 0; b < 6; ++b) {

            if (d.bar[b] != 0u && (d.bar[b] & 1u) == 0u) {
                mmio = d.bar[b] & ~0xFu;

                break;
            }
        }

        if (mmio == 0u) {
            kernel::serial::write("  no MMIO BAR found\n");
            continue;
        }

    #ifdef __x86_64__
        kernel::serial::write("  MMIO=0x"); kernel::serial::write_hex(mmio);
        kernel::serial::write(" (no MMIO mapping in x86_64 build)\n");
    #else
        const uint32_t map_size = 0x8000u; // 32 KiB
        void* base = map_physical_region(mmio, map_size);

        if (base == nullptr) {
            kernel::serial::write("  failed to map AHCI MMIO\n");
            continue;
        }

        volatile uint32_t* regs = reinterpret_cast<volatile uint32_t*>(base);
        const uint32_t cap = regs[0x00 / 4];
        const uint32_t pi = regs[0x0C / 4];

        kernel::serial::write("  CAP=0x"); kernel::serial::write_hex(cap);
        kernel::serial::write(" PI=0x"); kernel::serial::write_hex(pi);
        kernel::serial::write("\n");

        for (uint32_t port = 0; port < 32; ++port) {
            if ((pi & (1u << port)) == 0u) continue;

            volatile uint32_t* pbase = reinterpret_cast<volatile uint32_t*>((reinterpret_cast<volatile uint8_t*>(regs) + 0x100 + port * 0x80));
            const uint32_t sig = pbase[0x24 / 4];
            const uint32_t ssts = pbase[0x28 / 4];

            kernel::serial::write("    port "); kernel::serial::write_hex(port);
            kernel::serial::write(": SIG=0x"); kernel::serial::write_hex(sig);
            kernel::serial::write(" SSTS=0x"); kernel::serial::write_hex(ssts);
            kernel::serial::write("\n");
        }
#endif
    }
}

bool init() {
    kernel::pci::Device devs[64];
    uint32_t found = 0;

    if (!kernel::pci::scan_devices(devs, 64, &found) || found == 0) {
        return false;
    }

    for (uint32_t i = 0; i < found; ++i) {
        const kernel::pci::Device& d = devs[i];

        if (d.class_code != 0x01u || d.subclass != 0x06u) continue;

        uint32_t mmio = 0;

        for (int b = 0; b < 6; ++b) {
            if (d.bar[b] != 0u && (d.bar[b] & 1u) == 0u) {
                mmio = d.bar[b] & ~0xFu;
                break;
            }
        }

        if (mmio == 0u) continue;

        const uint32_t map_size = 0x8000u;
        void* base = map_physical_region(mmio, map_size);

        if (base == nullptr) continue;

        g_hba_regs = reinterpret_cast<volatile uint32_t*>(base);

        const uint32_t cap = g_hba_regs[0x00 / 4];
        const uint32_t pi = g_hba_regs[0x0C / 4];

        kernel::serial::write("ahci: CAP=0x"); kernel::serial::write_hex(cap);
        kernel::serial::write(" PI=0x"); kernel::serial::write_hex(pi);
        kernel::serial::write("\n");

        // allocate per-port structures but do not touch HBA control registers yet
        for (uint32_t port = 0; port < 32; ++port) {
            if ((pi & (1u << port)) == 0u) continue;

            volatile uint32_t* pbase = reinterpret_cast<volatile uint32_t*>((reinterpret_cast<volatile uint8_t*>(g_hba_regs) + 0x100 + port * 0x80));
            const uint32_t sig = pbase[0x24 / 4];
            const uint32_t ssts = pbase[0x28 / 4];

            kernel::serial::write("ahci: port "); kernel::serial::write_hex(port);
            kernel::serial::write(": SIG=0x"); kernel::serial::write_hex(sig);
            kernel::serial::write(" SSTS=0x"); kernel::serial::write_hex(ssts);
            kernel::serial::write("\n");

            // allocate CLB/FIS/CMD tables only on 32-bit builds; on x86_64 mapping
            // is handled differently by the platform (or left for later).
#ifndef __x86_64__
            void* clb = kernel::mm::heap::alloc(1024, 1024);
            void* fis = kernel::mm::heap::alloc(256, 256);
            void* cmd_tables = kernel::mm::heap::alloc(256 * 32, 1024);

            g_ports[port].regs = pbase;
            g_ports[port].clb = clb;
            g_ports[port].fis = fis;
            g_ports[port].cmd_tables = cmd_tables;
            g_ports[port].present = true;

            // start the port so CLB/FIS take effect
            start_port(port);
#else
            g_ports[port].regs = pbase;
            g_ports[port].clb = nullptr;
            g_ports[port].fis = nullptr;
            g_ports[port].cmd_tables = nullptr;
            g_ports[port].present = true;
#endif
        }

        // Register block device placeholder
        static kernel::BlockDevice ahci_block = {};

        ahci_block.name = "ahci0";
        ahci_block.block_size = 512;
        ahci_block.block_count = 0;
    #ifndef __x86_64__
        ahci_block.read_sectors = &ahci_read_sectors; // read implemented
        ahci_block.write_sectors = &ahci_write_sectors;
    #else
        ahci_block.read_sectors = nullptr; // x86_64: unimplemented
        ahci_block.write_sectors = nullptr;
    #endif
        ahci_block.ctx = nullptr;

        (void)kernel::block_register_device(&ahci_block);

        return true;
    }

    return false;
}
#ifndef __x86_64__
static void stop_port(uint32_t port) {
    if (port >= 32) return;
    if (!g_ports[port].present) return;

    volatile uint32_t* regs = g_ports[port].regs;
    if (regs == nullptr) return;

    // Clear ST (start) bit
    regs[0x18 / 4] &= ~0x1u;

    // Wait for CR (command list running) to clear (bit 15)
    while ((regs[0x18 / 4] & (1u << 15)) != 0u) {
        kernel::task::scheduler::sleep_current(1, kernel::arch::x86::interrupts::ticks());
    }
}

static void start_port(uint32_t port) {
    if (port >= 32) return;
    if (!g_ports[port].present) return;

    volatile uint32_t* regs = g_ports[port].regs;
    if (regs == nullptr) return;

    // Program CLB/FIS base addresses (low 32 bits)
    const uint32_t clb_phys = kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(g_ports[port].clb));
    const uint32_t fis_phys = kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(g_ports[port].fis));

    regs[0x00 / 4] = clb_phys;
    regs[0x04 / 4] = 0u;
    regs[0x08 / 4] = fis_phys;
    regs[0x0C / 4] = 0u;

    // Clear interrupt status
    regs[0x10 / 4] = regs[0x10 / 4];

    // Set FRE=1 (bit 4) to enable FIS receive, then set ST (bit 0)
    regs[0x18 / 4] |= (1u << 4);
    regs[0x18 / 4] |= 0x1u;

    // Small delay to let port start
    kernel::task::scheduler::sleep_current(1, kernel::arch::x86::interrupts::ticks());
}

#endif

#ifndef __x86_64__
// AHCI command structures
struct HbaCmdHeader {
    uint32_t dw0;
    uint32_t prdtl; // number of PRDT entries
    uint32_t prdbc;
    uint32_t ctba_low;
    uint32_t ctba_high;
    uint32_t reserved[4];
} __attribute__((packed));

struct HbaPrdtEntry {
    uint32_t dba_low;
    uint32_t dba_high;
    uint32_t dbc; // byte count (0-based) and IOC bit
    uint32_t reserved;
} __attribute__((packed));

struct HbaCmdTable {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    HbaPrdtEntry prdt[0]; // flexible
} __attribute__((packed));

static bool ahci_submit_read_port(uint32_t port, uint64_t lba, uint8_t* buf, uint32_t count) {
    if (port >= 32) return false;
    if (!g_ports[port].present) return false;
    if (buf == nullptr || count == 0) return false;

    volatile uint32_t* regs = g_ports[port].regs;
    if (regs == nullptr) return false;

    // command list is 1024 bytes, 32 bytes per header -> 32 headers
    void* clb = g_ports[port].clb;
    void* cmd_tables_base = g_ports[port].cmd_tables;

    // use slot 0 for simplicity
    const uint32_t slot = 0u;

    // clear command list
    memset(clb, 0, 1024);

    HbaCmdHeader* headers = reinterpret_cast<HbaCmdHeader*>(clb);
    HbaCmdHeader& hdr = headers[slot];

    // command table for this slot
    uint8_t* ct = reinterpret_cast<uint8_t*>(cmd_tables_base) + slot * 256;
    memset(ct, 0, 256);
    HbaCmdTable* table = reinterpret_cast<HbaCmdTable*>(ct);

    // Prepare PRDT entries to map buffer physical ranges
    const uint32_t bytes = count * 512u;
    uint32_t remaining = bytes;
    uint8_t* cur = buf;
    uint32_t prdt_count = 0;

    HbaPrdtEntry* prdt_entries = reinterpret_cast<HbaPrdtEntry*>(ct + offsetof(HbaCmdTable, prdt));

    while (remaining > 0 && prdt_count < 256) {
        uint32_t chunk = remaining;
        // align to page boundaries to be safe
        if (chunk > 0x100000) chunk = 0x100000; // limit 1 MiB per PRDT for simplicity

        const uint32_t phys = kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(cur));
        prdt_entries[prdt_count].dba_low = phys;
        prdt_entries[prdt_count].dba_high = 0;
        prdt_entries[prdt_count].dbc = (chunk - 1) & 0x3FFFFF; // 22 bits
        if (remaining <= chunk) prdt_entries[prdt_count].dbc |= (1u << 31); // IOC
        prdt_entries[prdt_count].reserved = 0;

        remaining -= chunk;
        cur += chunk;
        ++prdt_count;
    }

    if (remaining != 0) return false; // too many fragments

    // fill command table CFIS (Register - Host to Device)
    memset(table->cfis, 0, sizeof(table->cfis));
    table->cfis[0] = 0x27; // Host to device FIS
    table->cfis[1] = 1 << 7; // C bit? set to indicate command
    table->cfis[2] = 0x25; // READ DMA EXT

    // LBA (48-bit) into CFIS
    table->cfis[4] = static_cast<uint8_t>(lba & 0xFF);
    table->cfis[5] = static_cast<uint8_t>((lba >> 8) & 0xFF);
    table->cfis[6] = static_cast<uint8_t>((lba >> 16) & 0xFF);
    table->cfis[7] = static_cast<uint8_t>((lba >> 24) & 0xFF);
    table->cfis[8] = static_cast<uint8_t>((lba >> 32) & 0xFF);
    table->cfis[9] = static_cast<uint8_t>((lba >> 40) & 0xFF);

    // sector count (16-bit) in CFIS[12..13]
    uint16_t sc = static_cast<uint16_t>(count);
    table->cfis[12] = sc & 0xFF;
    table->cfis[13] = (sc >> 8) & 0xFF;

    // fill header
    hdr.dw0 = (5 & 0x1Fu); // CFIS length in DWORDS (5 -> 20 bytes)
    hdr.prdtl = prdt_count;
    hdr.prdbc = 0;
    const uint32_t ct_phys = kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(ct));
    hdr.ctba_low = ct_phys;
    hdr.ctba_high = 0;

    // ensure memory visibility
    asm volatile("mfence" ::: "memory");

    // issue command by setting PxCI bit
    regs[0x38 / 4] = (1u << slot);

    // wait for completion: PxCI bit clears
    while ((regs[0x38 / 4] & (1u << slot)) != 0u) {
        kernel::task::scheduler::sleep_current(1, kernel::arch::x86::interrupts::ticks());
    }

    // check TFDS for error
    uint32_t tfd = regs[0x20 / 4];
    if (tfd & 0x01) {
        // error
        return false;
    }

    return true;
}

static bool ahci_read_sectors(uint64_t lba, uint8_t* buf, uint32_t count) {
    if (buf == nullptr || count == 0) return false;

    // find first present port
    for (uint32_t port = 0; port < 32; ++port) {
        if (!g_ports[port].present) continue;
        if (g_ports[port].regs == nullptr) continue;

        // attempt read via AHCI DMA
        return ahci_submit_read_port(port, lba, buf, count);
    }

    return false;
}
#if !defined(__x86_64__)
static bool ahci_submit_write_port(uint32_t port, uint64_t lba, const uint8_t* buf, uint32_t count) {
    if (port >= 32) return false;
    if (!g_ports[port].present) return false;
    if (buf == nullptr || count == 0) return false;

    volatile uint32_t* regs = g_ports[port].regs;
    if (regs == nullptr) return false;

    void* clb = g_ports[port].clb;
    void* cmd_tables_base = g_ports[port].cmd_tables;

    const uint32_t slot = 0u;

    memset(clb, 0, 1024);

    HbaCmdHeader* headers = reinterpret_cast<HbaCmdHeader*>(clb);
    HbaCmdHeader& hdr = headers[slot];

    uint8_t* ct = reinterpret_cast<uint8_t*>(cmd_tables_base) + slot * 256;
    memset(ct, 0, 256);
    HbaCmdTable* table = reinterpret_cast<HbaCmdTable*>(ct);

    const uint32_t bytes = count * 512u;
    uint32_t remaining = bytes;
    const uint8_t* cur = buf;
    uint32_t prdt_count = 0;

    HbaPrdtEntry* prdt_entries = reinterpret_cast<HbaPrdtEntry*>(ct + offsetof(HbaCmdTable, prdt));

    while (remaining > 0 && prdt_count < 256) {
        uint32_t chunk = remaining;
        if (chunk > 0x100000) chunk = 0x100000;

        const uint32_t phys = kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(const_cast<uint8_t*>(cur)));
        prdt_entries[prdt_count].dba_low = phys;
        prdt_entries[prdt_count].dba_high = 0;
        prdt_entries[prdt_count].dbc = (chunk - 1) & 0x3FFFFF;
        if (remaining <= chunk) prdt_entries[prdt_count].dbc |= (1u << 31);
        prdt_entries[prdt_count].reserved = 0;

        remaining -= chunk;
        cur += chunk;
        ++prdt_count;
    }

    if (remaining != 0) return false;

    memset(table->cfis, 0, sizeof(table->cfis));
    table->cfis[0] = 0x27; // Host to device FIS
    table->cfis[1] = 1 << 7;
    table->cfis[2] = 0x35; // WRITE DMA EXT

    table->cfis[4] = static_cast<uint8_t>(lba & 0xFF);
    table->cfis[5] = static_cast<uint8_t>((lba >> 8) & 0xFF);
    table->cfis[6] = static_cast<uint8_t>((lba >> 16) & 0xFF);
    table->cfis[7] = static_cast<uint8_t>((lba >> 24) & 0xFF);
    table->cfis[8] = static_cast<uint8_t>((lba >> 32) & 0xFF);
    table->cfis[9] = static_cast<uint8_t>((lba >> 40) & 0xFF);

    uint16_t sc = static_cast<uint16_t>(count);
    table->cfis[12] = sc & 0xFF;
    table->cfis[13] = (sc >> 8) & 0xFF;

    hdr.dw0 = (5 & 0x1Fu);
    hdr.prdtl = prdt_count;
    hdr.prdbc = 0;
    const uint32_t ct_phys = kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(ct));
    hdr.ctba_low = ct_phys;
    hdr.ctba_high = 0;

    asm volatile("mfence" ::: "memory");

    regs[0x38 / 4] = (1u << slot);

    while ((regs[0x38 / 4] & (1u << slot)) != 0u) {
        kernel::task::scheduler::sleep_current(1, kernel::arch::x86::interrupts::ticks());
    }

    uint32_t tfd = regs[0x20 / 4];
    if (tfd & 0x01) {
        return false;
    }

    return true;
}

static bool ahci_write_sectors(uint64_t lba, const uint8_t* buf, uint32_t count) {
    if (buf == nullptr || count == 0) return false;

    for (uint32_t port = 0; port < 32; ++port) {
        if (!g_ports[port].present) continue;
        if (g_ports[port].regs == nullptr) continue;

        // submit write
        return ahci_submit_write_port(port, lba, buf, count);
    }

    return false;
}
#endif
#endif


} // namespace ahci
} // namespace kernel
