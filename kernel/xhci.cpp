#include "xhci.hpp"

#include "pci.hpp"
#ifndef __x86_64__
#include "mm/heap.hpp"
#include "mm/vmm.hpp"
#endif
#include "serial.hpp"
#include "task/scheduler.hpp"
#ifdef __x86_64__
#include "arch/x86_64/interrupts.hpp"
#else
#include "arch/x86/interrupts.hpp"
#include "arch/x86/pic.hpp"
#endif

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kPageSize = 4096u;
constexpr uint64_t kMmioVirtBase = 0xFFFF800000000000ull;
constexpr uint64_t kMmioPageSize = 2ull * 1024ull * 1024ull;

constexpr uint64_t kPtePresent = 1ull << 0;
constexpr uint64_t kPteWrite = 1ull << 1;
constexpr uint64_t kPtePwt = 1ull << 3;
constexpr uint64_t kPtePcd = 1ull << 4;
constexpr uint64_t kPtePs = 1ull << 7;

constexpr uint32_t kTrbTypeLink = 6;
constexpr uint32_t kTrbTypeEnableSlot = 9;
constexpr uint32_t kTrbTypeAddressDevice = 11;
constexpr uint32_t kTrbTypeConfigureEndpoint = 12;
constexpr uint32_t kTrbTypeSetupStage = 2;
constexpr uint32_t kTrbTypeDataStage = 3;
constexpr uint32_t kTrbTypeStatusStage = 4;
constexpr uint32_t kTrbTypeNormal = 1;
constexpr uint32_t kTrbTypeCommandCompletion = 33;
constexpr uint32_t kTrbTypeTransferEvent = 32;

constexpr uint32_t kTrbCycle = 1u << 0;
constexpr uint32_t kTrbCh = 1u << 4;
constexpr uint32_t kTrbIoc = 1u << 5;
constexpr uint32_t kTrbIdt = 1u << 6;
constexpr uint32_t kTrbDirIn = 1u << 16;
constexpr uint32_t kTrbLinkTc = 1u << 1;

constexpr uint32_t kPortscCcs = 1u << 0;
constexpr uint32_t kPortscPed = 1u << 1;
constexpr uint32_t kPortscPr = 1u << 4;
constexpr uint32_t kPortscCsc = 1u << 17;
constexpr uint32_t kPortscPrc = 1u << 21;

constexpr uint32_t kUsbcmdRun = 1u << 0;
constexpr uint32_t kUsbcmdHcrst = 1u << 1;
constexpr uint32_t kUsbcmdInth = 1u << 2;
constexpr uint32_t kUsbstsHch = 1u << 0;
constexpr uint32_t kUsbstsCnr = 1u << 11;

constexpr uint8_t kUsbClassMassStorage = 0x08;
constexpr uint8_t kUsbSubclassScsi = 0x06;
constexpr uint8_t kUsbProtoBulk = 0x50;

constexpr uint32_t kRingSize = 256;

struct Trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

struct Ring {
    Trb* trbs;
    uint32_t size;
    uint32_t index;
    uint8_t cycle;
    bool has_link;
};

struct Controller {
    volatile uint8_t* base;
    volatile uint32_t* op;
    volatile uint32_t* rt;
    volatile uint32_t* db;
    uint32_t caplen;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t ctx_size;
    Ring cmd_ring;
    Ring evt_ring;
    Trb* evt_trbs;
    uint32_t evt_index;
    uint8_t evt_cycle;
    uint64_t* dcbaa;
    bool ready;
};

static Controller g_ctrl = {};

struct DeviceState {
    kernel::xhci::Device dev;
    Ring ep0_ring;
    Ring bulk_in_ring;
    Ring bulk_out_ring;
    uint8_t* dev_ctx;
    uint8_t* input_ctx;
    bool configured;
};

static DeviceState g_dev = {};

struct CompletionEntry {
    volatile uint64_t ptr;
    uint8_t code;
    Trb ev;
};

static CompletionEntry g_completions[128];

alignas(4096) static uint8_t g_xhci_heap[256 * 1024] = {};
static size_t g_xhci_heap_used = 0;

static void* xhci_alloc(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
    if (alignment == 0) alignment = 1;

    const size_t mask = alignment - 1u;
    const size_t aligned = (g_xhci_heap_used + mask) & ~mask;

    if (aligned + size > sizeof(g_xhci_heap)) return nullptr;

    g_xhci_heap_used = aligned + size;

    return g_xhci_heap + aligned;
}

[[maybe_unused]] static uint64_t align_down(uint64_t value, uint64_t align) {
    return value & ~(align - 1u);
}

[[maybe_unused]]
static uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static void zero_mem(void* dst, size_t bytes) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dst);

    for (size_t i = 0; i < bytes; ++i) {
        d[i] = 0;
    }
}

static void cpu_pause() {
    asm volatile("pause" ::: "memory");
}

[[maybe_unused]] static void invlpg(void* addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

// IRQ handler wrapper called from interrupt dispatcher
[[maybe_unused]] static void xhci_irq_handler() {
    kernel::xhci::handle_irq();
}

#ifndef __x86_64__
static void* map_physical_region(uint64_t phys, uint32_t size) {
    const uint32_t page = kPageSize;
    const uint32_t pages = (size + page - 1u) / page;

    void* virt = kernel::mm::heap::alloc(static_cast<size_t>(pages) * page, page);

    if (virt == nullptr) return nullptr;

    uint64_t v = reinterpret_cast<uintptr_t>(virt);
    uint64_t p = phys & ~(static_cast<uint64_t>(page) - 1u);

    for (uint32_t i = 0; i < pages; ++i) {
        if (!kernel::mm::vmm::map_page(static_cast<uint32_t>(v + i * page), static_cast<uint32_t>(p + i * page), true, false)) {
            return nullptr;
        }
    }

    return virt;
}
#else
extern "C" uint64_t pml4[];

static uint64_t* g_mmio_pdpt = nullptr;
static uint64_t* g_mmio_pd = nullptr;
static uint64_t g_mmio_next = kMmioVirtBase;

static void reload_cr3() {
    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static bool ensure_mmio_tables() {
    const uint64_t pml4_index = (kMmioVirtBase >> 39) & 0x1FFu;

    if ((pml4[pml4_index] & kPtePresent) == 0) {
        g_mmio_pdpt = reinterpret_cast<uint64_t*>(xhci_alloc(kPageSize, kPageSize));

        if (g_mmio_pdpt == nullptr) return false;

        zero_mem(g_mmio_pdpt, kPageSize);

        pml4[pml4_index] = (reinterpret_cast<uint64_t>(g_mmio_pdpt) & ~0xFFFull) | kPtePresent | kPteWrite;

    } else {
        g_mmio_pdpt = reinterpret_cast<uint64_t*>(pml4[pml4_index] & ~0xFFFull);
    }

    if (g_mmio_pd == nullptr) {
        g_mmio_pd = reinterpret_cast<uint64_t*>(xhci_alloc(kPageSize, kPageSize));

        if (g_mmio_pd == nullptr) return false;
        
        zero_mem(g_mmio_pd, kPageSize);
        
        g_mmio_pdpt[0] = (reinterpret_cast<uint64_t>(g_mmio_pd) & ~0xFFFull) | kPtePresent | kPteWrite;
    }

    return true;
}

static void* map_physical_region(uint64_t phys, uint32_t size) {
    if (!ensure_mmio_tables()) return nullptr;

    const uint64_t phys_base = align_down(phys, kMmioPageSize);
    const uint64_t offset = phys - phys_base;
    const uint64_t map_size = static_cast<uint64_t>(size) + offset;
    const uint64_t pages = (map_size + kMmioPageSize - 1u) / kMmioPageSize;

    uint64_t vbase = align_up(g_mmio_next, kMmioPageSize);
    g_mmio_next = vbase + pages * kMmioPageSize;

    const uint64_t pd_base_index = (vbase >> 21) & 0x1FFu;
    
    if (pd_base_index + pages > 512u) {
        return nullptr;
    }

    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t vaddr = vbase + i * kMmioPageSize;
        const uint64_t paddr = phys_base + i * kMmioPageSize;
    
        g_mmio_pd[pd_base_index + i] = (paddr & ~0x1FFFFFull) | kPtePresent | kPteWrite | kPtePwt | kPtePcd | kPtePs;
        invlpg(reinterpret_cast<void*>(vaddr));
    }

    reload_cr3();

    return reinterpret_cast<void*>(vbase + offset);
}

#endif

static void ring_init(Ring* r, uint32_t count, bool link) {
    r->trbs = reinterpret_cast<Trb*>(xhci_alloc(sizeof(Trb) * count, 16));

    r->size = count;
    r->index = 0;
    r->cycle = 1;
    r->has_link = link;

    if (r->trbs != nullptr) {
        zero_mem(r->trbs, sizeof(Trb) * count);
    }

    if (link && r->trbs != nullptr) {
        Trb* link_trb = &r->trbs[count - 1u];

        link_trb->parameter = reinterpret_cast<uint64_t>(r->trbs);
        link_trb->status = 0;
        link_trb->control = (kTrbTypeLink << 10) | kTrbLinkTc | (r->cycle ? kTrbCycle : 0);
    }
}

static Trb* ring_alloc_trb(Ring* r) {
    if (r->trbs == nullptr) return nullptr;

    if (r->has_link && r->index >= r->size - 1u) {
        Trb* link_trb = &r->trbs[r->size - 1u];

        link_trb->control = (kTrbTypeLink << 10) | kTrbLinkTc | (r->cycle ? kTrbCycle : 0);

        r->index = 0;
        r->cycle ^= 1u;
    }

    Trb* trb = &r->trbs[r->index++];

    zero_mem(trb, sizeof(Trb));

    return trb;
}

static void ring_doorbell(uint32_t slot_id, uint32_t target) {
    g_ctrl.db[slot_id] = target;
}

static bool wait_reg(volatile uint32_t* reg, uint32_t mask, bool set, uint32_t spins) {
    while (spins-- > 0) {
        const uint32_t val = *reg;

        if (((val & mask) != 0u) == set) return true;

        cpu_pause();
    }
    return false;
}

static bool event_ready() {
    Trb* trb = &g_ctrl.evt_trbs[g_ctrl.evt_index];
    return ((trb->control & kTrbCycle) != 0u) == (g_ctrl.evt_cycle != 0u);
}

static Trb next_event() {
    Trb trb = g_ctrl.evt_trbs[g_ctrl.evt_index];
    g_ctrl.evt_index++;

    if (g_ctrl.evt_index >= g_ctrl.evt_ring.size) {
        g_ctrl.evt_index = 0;
        g_ctrl.evt_cycle ^= 1u;
    }

    const uint64_t erdp = reinterpret_cast<uint64_t>(&g_ctrl.evt_trbs[g_ctrl.evt_index]);
    volatile uint32_t* intr = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(g_ctrl.rt) + 0x20u);

    intr[0x18 / 4] = static_cast<uint32_t>(erdp) | 0x8u;
    intr[0x1C / 4] = static_cast<uint32_t>(erdp >> 32);

    return trb;
}

static bool wait_for_event(uint32_t type, uint64_t ptr, Trb* out_event, uint8_t* completion) {
    (void)type;
    uint64_t spins = 0;

    for (;;) {
        for (int i = 0; i < 128; ++i) {
            volatile uint64_t p = g_completions[i].ptr;

            if (p == ptr) {
                uint8_t code = g_completions[i].code;
                if (out_event) *out_event = g_completions[i].ev;
                if (completion) *completion = code;

                g_completions[i].ptr = 0;
                g_completions[i].code = 0;
                return code == 1u;
            }
        }

        kernel::xhci::handle_irq();
        cpu_pause();

        if (++spins >= 50000000u) {
            kernel::serial::write("[xhci]: wait timeout\n");
            return false;
        }
    }
}

static bool submit_command(Trb* trb, Trb* out_event, uint8_t* completion) {
    const uint64_t ptr = reinterpret_cast<uint64_t>(trb);

    kernel::serial::write("[xhci]: cmd doorbell\n");
    ring_doorbell(0, 0);

    return wait_for_event(kTrbTypeCommandCompletion, ptr, out_event, completion);
}

static bool submit_transfer(uint8_t slot_id, uint8_t dci, Trb* trb, uint8_t* completion) {
    const uint64_t ptr = reinterpret_cast<uint64_t>(trb);

    kernel::serial::write("[xhci]: xfer doorbell slot=");
    kernel::serial::write_hex(slot_id);
    kernel::serial::write(" dci=");
    kernel::serial::write_hex(dci);
    kernel::serial::write("\n");
    ring_doorbell(slot_id, dci);

    return wait_for_event(kTrbTypeTransferEvent, ptr, nullptr, completion);
}

static uint8_t port_speed(uint32_t portsc) {
    return static_cast<uint8_t>((portsc >> 10) & 0x0Fu);
}

static uint16_t default_max_packet(uint8_t speed) {
    switch (speed) {
        case 1: return 8;   // low speed
        case 2: return 8;   // full speed
        case 3: return 64;  // high speed
        case 4: return 512; // super speed
        default: return 8;
    }
}

static void write_slot_context(uint8_t* ctx, uint8_t speed, uint8_t port, uint8_t ctx_entries) {
    uint32_t* dw = reinterpret_cast<uint32_t*>(ctx);

    dw[0] = (static_cast<uint32_t>(speed) << 20) | (static_cast<uint32_t>(ctx_entries) << 27);
    dw[1] = (static_cast<uint32_t>(port) << 16);
}

static void write_ep0_context(uint8_t* ctx, uint64_t tr_deq, uint16_t mps) {
    uint32_t* dw = reinterpret_cast<uint32_t*>(ctx);

    dw[0] = 0;
    dw[1] = (3u << 1) | (4u << 3) | (static_cast<uint32_t>(mps) << 16);

    dw[2] = static_cast<uint32_t>(tr_deq & 0xFFFFFFFFu);
    dw[3] = static_cast<uint32_t>(tr_deq >> 32) | 1u;

    dw[4] = 8;
}

static void write_ep_context(uint8_t* ctx, uint64_t tr_deq, uint16_t mps, uint8_t ep_type) {
    uint32_t* dw = reinterpret_cast<uint32_t*>(ctx);
    dw[0] = 0;
    dw[1] = (3u << 1) | (static_cast<uint32_t>(ep_type) << 3) | (static_cast<uint32_t>(mps) << 16);
    
    dw[2] = static_cast<uint32_t>(tr_deq & 0xFFFFFFFFu);
    dw[3] = static_cast<uint32_t>(tr_deq >> 32) | 1u;
    
    dw[4] = 8;
}

} // namespace

namespace kernel {
namespace xhci {

bool ready() {
    return g_ctrl.ready;
}

static bool controller_init(uint64_t mmio, const kernel::pci::Device& dev) {
    kernel::serial::write("[xhci]: controller init\n");

    const uint32_t map_size = 0x10000u;
    
    void* base = map_physical_region(mmio, map_size);
    
    if (base == nullptr) {
        kernel::serial::write("[xhci]: failed to map MMIO\n");
    
        return false;
    }

    kernel::serial::write("[xhci]: mmio mapped\n");

    g_ctrl.base = reinterpret_cast<volatile uint8_t*>(base);
    g_ctrl.caplen = g_ctrl.base[0];
    g_ctrl.op = reinterpret_cast<volatile uint32_t*>(g_ctrl.base + g_ctrl.caplen);
    
    const uint32_t hcs1 = *reinterpret_cast<volatile uint32_t*>(g_ctrl.base + 0x04);
    const uint32_t hcs2 = *reinterpret_cast<volatile uint32_t*>(g_ctrl.base + 0x08);
    const uint32_t hcc1 = *reinterpret_cast<volatile uint32_t*>(g_ctrl.base + 0x10);

    kernel::serial::write("[xhci]: regs read\n");
    
    g_ctrl.max_slots = hcs1 & 0xFFu;
    g_ctrl.max_ports = (hcs1 >> 24) & 0xFFu;
    g_ctrl.ctx_size = (hcc1 & (1u << 2)) ? 64u : 32u;

    g_ctrl.dboff = *reinterpret_cast<volatile uint32_t*>(g_ctrl.base + 0x14);
    g_ctrl.rtsoff = *reinterpret_cast<volatile uint32_t*>(g_ctrl.base + 0x18);
    
    g_ctrl.db = reinterpret_cast<volatile uint32_t*>(g_ctrl.base + (g_ctrl.dboff & ~0x3u));
    g_ctrl.rt = reinterpret_cast<volatile uint32_t*>(g_ctrl.base + (g_ctrl.rtsoff & ~0x1Fu));

    g_ctrl.op[0x00 / 4] &= ~kUsbcmdRun;
    
    (void)wait_reg(&g_ctrl.op[0x04 / 4], kUsbstsHch, true, 1000000u);

    g_ctrl.op[0x00 / 4] |= kUsbcmdHcrst;

    kernel::serial::write("[xhci]: controller reset\n");
    
    if (!wait_reg(&g_ctrl.op[0x00 / 4], kUsbcmdHcrst, false, 2000000u)) {
        kernel::serial::write("[xhci]: reset timeout\n");
    
        return false;
    
    }
    
    if (!wait_reg(&g_ctrl.op[0x04 / 4], kUsbstsCnr, false, 2000000u)) {
        kernel::serial::write("[xhci]: controller not ready\n");
    
        return false;
    }

    ring_init(&g_ctrl.cmd_ring, kRingSize, true);
    
    if (g_ctrl.cmd_ring.trbs == nullptr) return false;

    ring_init(&g_ctrl.evt_ring, kRingSize, false);
    
    g_ctrl.evt_trbs = g_ctrl.evt_ring.trbs;
    g_ctrl.evt_index = 0;
    g_ctrl.evt_cycle = 1;

    g_ctrl.dcbaa = reinterpret_cast<uint64_t*>(xhci_alloc(sizeof(uint64_t) * (g_ctrl.max_slots + 1u), 64));
    
    if (g_ctrl.dcbaa == nullptr) return false;
    
    zero_mem(g_ctrl.dcbaa, sizeof(uint64_t) * (g_ctrl.max_slots + 1u));

    const uint32_t scratch_lo = hcs2 & 0x1Fu;
    const uint32_t scratch_hi = (hcs2 >> 21) & 0x1Fu;
    const uint32_t scratch_cnt = scratch_lo | (scratch_hi << 5);
    
    if (scratch_cnt > 0) {
    
        uint64_t* scratch_array = reinterpret_cast<uint64_t*>(xhci_alloc(sizeof(uint64_t) * scratch_cnt, 64));
    
        if (scratch_array == nullptr) return false;
    
        for (uint32_t i = 0; i < scratch_cnt; ++i) {
            void* buf = xhci_alloc(kPageSize, kPageSize);
    
            if (buf == nullptr) return false;
    
            scratch_array[i] = reinterpret_cast<uint64_t>(buf);
        }
    
        g_ctrl.dcbaa[0] = reinterpret_cast<uint64_t>(scratch_array);
    }

    g_ctrl.op[0x18 / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(g_ctrl.cmd_ring.trbs)) | g_ctrl.cmd_ring.cycle;
    g_ctrl.op[0x1C / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(g_ctrl.cmd_ring.trbs) >> 32);

    g_ctrl.op[0x30 / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(g_ctrl.dcbaa));
    g_ctrl.op[0x34 / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(g_ctrl.dcbaa) >> 32);

    struct ErSegment {
        uint64_t base;
        uint32_t size;
        uint32_t rsvd;
    
    } __attribute__((packed));

    ErSegment* erst = reinterpret_cast<ErSegment*>(xhci_alloc(sizeof(ErSegment), 64));
    if (erst == nullptr) return false;
    
    erst[0].base = reinterpret_cast<uint64_t>(g_ctrl.evt_trbs);
    erst[0].size = g_ctrl.evt_ring.size;
    erst[0].rsvd = 0;

    volatile uint32_t* intr = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(g_ctrl.rt) + 0x20u);
    
    intr[0x08 / 4] = 1; // ERSTSZ
    intr[0x10 / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(erst));
    intr[0x14 / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(erst) >> 32);
    intr[0x18 / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(g_ctrl.evt_trbs));
    intr[0x1C / 4] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(g_ctrl.evt_trbs) >> 32);
    intr[0x00 / 4] = 1; // IMAN.IE

    g_ctrl.op[0x38 / 4] = g_ctrl.max_slots & 0xFFu;
    g_ctrl.op[0x00 / 4] |= kUsbcmdRun | kUsbcmdInth;

    if (!wait_reg(&g_ctrl.op[0x04 / 4], kUsbstsHch, false, 1000000u)) {
    
        kernel::serial::write("[xhci]: failed to start\n");
    
        return false;
    }

    g_ctrl.ready = true;
    kernel::serial::write("[xhci]: controller ready\n");
    (void)dev;
    
    return true;
}

bool init() {
    if (g_ctrl.ready) return true;

    kernel::pci::Device devs[64];
    uint32_t found = 0;

    if (!kernel::pci::scan_devices(devs, 64, &found) || found == 0) {

        kernel::serial::write("[xhci]: no PCI devices found\n");

        return false;
    }

    for (uint32_t i = 0; i < found; ++i) {
        const kernel::pci::Device& d = devs[i];
        if (d.class_code != 0x0Cu || d.subclass != 0x03u || d.prog_if != 0x30u) continue;

        kernel::serial::write("[xhci]: pci ");
        kernel::serial::write_hex(d.bus);
        kernel::serial::write(":");
        kernel::serial::write_hex(d.slot);
        kernel::serial::write(".");
        kernel::serial::write_hex(d.func);
        kernel::serial::write(" class=");
        kernel::serial::write_hex(d.class_code);
        kernel::serial::write(" sub=");
        kernel::serial::write_hex(d.subclass);
        kernel::serial::write(" pi=");
        kernel::serial::write_hex(d.prog_if);
        kernel::serial::write(" bars=");
        for (int b = 0; b < 6; ++b) {
            kernel::serial::write_hex(d.bar[b]);
            kernel::serial::write(" ");
        }
        kernel::serial::write("\n");

        uint16_t cmd = kernel::pci::config_read16(d.bus, d.slot, d.func, 0x04);
        cmd |= 0x6u; // memory + bus master

        kernel::pci::config_write16(d.bus, d.slot, d.func, 0x04, cmd);

        uint64_t mmio = 0;
        for (int b = 0; b < 6; ++b) {

            uint32_t bar = d.bar[b];

            if (bar == 0u) continue;
            if ((bar & 1u) != 0u) continue;

            if ((bar & 0x6u) == 0x4u && b + 1 < 6) {
                uint64_t low = static_cast<uint64_t>(bar & ~0xFu);
                uint64_t high = static_cast<uint64_t>(d.bar[b + 1]);

                mmio = (high << 32) | low;

                break;
            }

            mmio = static_cast<uint64_t>(bar & ~0xFu);

            break;
        }

        if (mmio == 0u) {
            kernel::serial::write("[xhci]: no MMIO BAR found\n");
            continue;
        }

        kernel::serial::write("[xhci]: MMIO=0x");
        kernel::serial::write_hex(static_cast<uint32_t>(mmio));

        kernel::serial::write("\n");

        const bool ok = controller_init(mmio, d);
        if (ok) {
        #ifndef __x86_64__
            const uint8_t vector = static_cast<uint8_t>(0x20u + d.irq);
            kernel::arch::x86::interrupts::register_irq_handler(vector, xhci_irq_handler);
            kernel::arch::x86::pic::clear_irq_mask(d.irq);
        #endif
        }

        return ok;
    }

    kernel::serial::write("[xhci]: no xHCI controller found\n");
    return false;
}

static bool reset_port(uint8_t port) {

    if (port == 0) return false;

    volatile uint32_t* portsc = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(g_ctrl.op) + 0x400 + (port - 1u) * 0x10);

    uint32_t v = *portsc;

    if ((v & kPortscCcs) == 0u) return false;

    *portsc = v | kPortscPr;

    for (uint32_t spins = 0; spins < 1000000u; ++spins) {
        v = *portsc;

        if ((v & kPortscPr) == 0u) break;
        cpu_pause();
    }
    v = *portsc;
    *portsc = v | kPortscPrc | kPortscCsc;

    return (v & kPortscPed) != 0u;
}

static bool address_device(uint8_t port, Device* out) {
    Trb* cmd = ring_alloc_trb(&g_ctrl.cmd_ring);
    if (cmd == nullptr) return false;

    cmd->control = (kTrbTypeEnableSlot << 10) | (g_ctrl.cmd_ring.cycle ? kTrbCycle : 0);
    uint8_t cc = 0;
    Trb ev = {};

    if (!submit_command(cmd, &ev, &cc)) return false;

    const uint8_t slot_id = static_cast<uint8_t>((ev.control >> 24) & 0xFFu);

    if (slot_id == 0 || slot_id > g_ctrl.max_slots) return false;

    const uint32_t ctx_bytes = g_ctrl.ctx_size;
    const uint32_t max_ctx = 32;

    g_dev.dev_ctx = reinterpret_cast<uint8_t*>(xhci_alloc(ctx_bytes * max_ctx, 64));
    g_dev.input_ctx = reinterpret_cast<uint8_t*>(xhci_alloc(ctx_bytes * (max_ctx + 1u), 64));

    if (g_dev.dev_ctx == nullptr || g_dev.input_ctx == nullptr) return false;

    zero_mem(g_dev.dev_ctx, ctx_bytes * max_ctx);
    zero_mem(g_dev.input_ctx, ctx_bytes * (max_ctx + 1u));

    g_ctrl.dcbaa[slot_id] = reinterpret_cast<uint64_t>(g_dev.dev_ctx);

    uint8_t* icc = g_dev.input_ctx;
    uint32_t* add_flags = reinterpret_cast<uint32_t*>(icc + 4);

    *add_flags = 0x3u; // slot + ep0

    const uint8_t speed = port_speed(*reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(g_ctrl.op) + 0x400 + (port - 1u) * 0x10));
    const uint16_t mps = default_max_packet(speed);

    uint8_t* slot_ctx = g_dev.input_ctx + ctx_bytes;
    uint8_t* ep0_ctx = g_dev.input_ctx + ctx_bytes * 2u;

    write_slot_context(slot_ctx, speed, port, 1);

    ring_init(&g_dev.ep0_ring, kRingSize, true);

    if (g_dev.ep0_ring.trbs == nullptr) return false;

    write_ep0_context(ep0_ctx, reinterpret_cast<uint64_t>(g_dev.ep0_ring.trbs), mps);

    Trb* addr = ring_alloc_trb(&g_ctrl.cmd_ring);

    if (addr == nullptr) return false;

    addr->parameter = reinterpret_cast<uint64_t>(g_dev.input_ctx);
    addr->control = (kTrbTypeAddressDevice << 10) | (static_cast<uint32_t>(slot_id) << 24) | (g_ctrl.cmd_ring.cycle ? kTrbCycle : 0);

    if (!submit_command(addr, nullptr, &cc)) return false;

    kernel::serial::write("[xhci]: address slot=");
    kernel::serial::write_hex(slot_id);
    kernel::serial::write(" cc=");
    kernel::serial::write_hex(cc);
    kernel::serial::write("\n");

    g_dev.dev.slot_id = slot_id;
    g_dev.dev.port_id = port;

    g_dev.dev.speed = speed;
    g_dev.dev.ep0.dci = 1;

    g_dev.dev.ep0.max_packet = mps;

    g_dev.dev.ep0.in = true;
    g_dev.configured = false;

    *out = g_dev.dev;

    return true;
}

bool control_transfer(const Device& dev, const UsbSetupPacket& setup, void* data, uint32_t length) {
    if (!g_ctrl.ready) return false;

    Ring* ring = &g_dev.ep0_ring;

    if (ring->trbs == nullptr) return false;

    const uint32_t trt = (length == 0) ? 0u : ((setup.bm_request_type & 0x80u) ? 3u : 2u);

    Trb* setup_trb = ring_alloc_trb(ring);

    const uint32_t setup_cycle = ring->cycle ? kTrbCycle : 0;

    setup_trb->parameter = static_cast<uint64_t>(setup.bm_request_type) |
                           (static_cast<uint64_t>(setup.b_request) << 8) |
                           (static_cast<uint64_t>(setup.w_value) << 16) |
                           (static_cast<uint64_t>(setup.w_index) << 32) |
                           (static_cast<uint64_t>(setup.w_length) << 48);

    setup_trb->status = 8;
    setup_trb->control = (kTrbTypeSetupStage << 10) | kTrbIdt | (trt << 16) | (length ? kTrbCh : 0) | setup_cycle;

    if (length > 0) {
        Trb* data_trb = ring_alloc_trb(ring);
        const uint32_t data_cycle = ring->cycle ? kTrbCycle : 0;
    
        data_trb->parameter = reinterpret_cast<uint64_t>(data);
        data_trb->status = length;
    
        uint32_t ctrl = (kTrbTypeDataStage << 10) | kTrbCh | data_cycle;
    
        if (setup.bm_request_type & 0x80u) {
            ctrl |= kTrbDirIn;
        }
    
        data_trb->control = ctrl;
    }

    Trb* status_trb = ring_alloc_trb(ring);
    const uint32_t status_cycle = ring->cycle ? kTrbCycle : 0;
    
    status_trb->parameter = 0;
    status_trb->status = 0;
    
    uint32_t status_ctrl = (kTrbTypeStatusStage << 10) | kTrbIoc | status_cycle;
    
    if ((setup.bm_request_type & 0x80u) == 0u) {
        status_ctrl |= kTrbDirIn;
    }
    
    status_trb->control = status_ctrl;

    uint8_t cc = 0;
    const bool ok = submit_transfer(dev.slot_id, dev.ep0.dci, status_trb, &cc);

    if (!ok) {
        kernel::serial::write("[xhci]: control transfer failed cc=");
        kernel::serial::write_hex(cc);
        kernel::serial::write(" dci=");
        kernel::serial::write_hex(dev.ep0.dci);
        kernel::serial::write("\n");
    }

    return ok;
}

bool bulk_transfer(const Device& dev, const Endpoint& ep, void* data, uint32_t length) {
    if (!g_ctrl.ready) return false;

    Ring* ring = nullptr;

    if (ep.dci == g_dev.dev.bulk_in.dci) {
        ring = &g_dev.bulk_in_ring;
    
    } else if (ep.dci == g_dev.dev.bulk_out.dci) {
        ring = &g_dev.bulk_out_ring;
    
    } else {
        return false;
    }

    if (ring->trbs == nullptr) return false;

    Trb* trb = ring_alloc_trb(ring);
    const uint32_t cycle = ring->cycle ? kTrbCycle : 0;
    
    trb->parameter = reinterpret_cast<uint64_t>(data);
    trb->status = length;
    
    trb->control = (kTrbTypeNormal << 10) | kTrbIoc | cycle;

    uint8_t cc = 0;
    
    return submit_transfer(dev.slot_id, ep.dci, trb, &cc);
}

bool get_mass_storage_device(Device* out) {
    if (out == nullptr) return false;
    if (!g_ctrl.ready && !init()) return false;

    uint8_t found_port = 0;
    
    for (uint32_t p = 1; p <= g_ctrl.max_ports; ++p) {
        volatile uint32_t* portsc = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(g_ctrl.op) + 0x400 + (p - 1u) * 0x10);
        uint32_t v = *portsc;
    
        if ((v & kPortscCcs) != 0u) {
            found_port = static_cast<uint8_t>(p);
            break;
        }
    }

    if (found_port == 0) {
        kernel::serial::write("[xhci]: no connected ports\n");
        return false;
    }

    if (!reset_port(found_port)) {
        kernel::serial::write("[xhci]: port reset failed\n");
        return false;
    }

    Device dev = {};
    
    if (!address_device(found_port, &dev)) {
        kernel::serial::write("[xhci]: address device failed\n");
        return false;
    }

    uint8_t dev_desc[18] = {};
    UsbSetupPacket get_dev = {0x80, 6, static_cast<uint16_t>(1u << 8), 0, sizeof(dev_desc)};

    if (!control_transfer(dev, get_dev, dev_desc, sizeof(dev_desc))) {
        kernel::serial::write("[xhci]: get device descriptor failed\n");
        return false;
    }

    uint8_t cfg_hdr[9] = {};
    UsbSetupPacket get_cfg_hdr = {0x80, 6, static_cast<uint16_t>(2u << 8), 0, sizeof(cfg_hdr)};

    if (!control_transfer(dev, get_cfg_hdr, cfg_hdr, sizeof(cfg_hdr))) {
        kernel::serial::write("[xhci]: get config header failed\n");
        return false;
    }

    const uint16_t total_len = static_cast<uint16_t>(cfg_hdr[2]) | (static_cast<uint16_t>(cfg_hdr[3]) << 8);
    if (total_len < sizeof(cfg_hdr) || total_len > 512) {
        kernel::serial::write("[xhci]: invalid config length\n");
        return false;
    }

    uint8_t cfg_buf[512] = {};
    UsbSetupPacket get_cfg = {0x80, 6, static_cast<uint16_t>(2u << 8), 0, total_len};

    if (!control_transfer(dev, get_cfg, cfg_buf, total_len)) {
        kernel::serial::write("[xhci]: get config failed\n");
        return false;
    }

    uint8_t cfg_value = cfg_buf[5];

    uint8_t bulk_in_ep = 0;
    uint8_t bulk_out_ep = 0;

    uint16_t idx = 0;

    while (idx + 1 < total_len) {
        const uint8_t len = cfg_buf[idx];

        const uint8_t type = cfg_buf[idx + 1];

        if (len == 0) break;

        if (type == 4 && idx + len <= total_len) {
            const uint8_t iface_class = cfg_buf[idx + 5];
            const uint8_t iface_sub = cfg_buf[idx + 6];
            const uint8_t iface_proto = cfg_buf[idx + 7];

            if (iface_class == kUsbClassMassStorage && iface_sub == kUsbSubclassScsi && iface_proto == kUsbProtoBulk) {
                // scan endpoints within this interface
                uint16_t eidx = idx + len;

                while (eidx + 1 < total_len) {
                    const uint8_t elen = cfg_buf[eidx];
                    const uint8_t etype = cfg_buf[eidx + 1];

                    if (elen == 0) break;

                    if (etype == 5 && eidx + elen <= total_len) {
                        const uint8_t addr = cfg_buf[eidx + 2];
                        const uint8_t attrs = cfg_buf[eidx + 3];

                        if ((attrs & 0x3u) == 2u) {
                            if (addr & 0x80u) bulk_in_ep = addr & 0x0Fu;
                            else bulk_out_ep = addr & 0x0Fu;
                        }

                    } else if (etype == 4) {
                        break;

                    }

                    eidx = static_cast<uint16_t>(eidx + elen);
                }
            }
        }

        idx = static_cast<uint16_t>(idx + len);
    }

    if (bulk_in_ep == 0 || bulk_out_ep == 0) {
        kernel::serial::write("[xhci]: bulk endpoints not found\n");
        return false;
    }

    UsbSetupPacket set_cfg = {0x00, 9, cfg_value, 0, 0};
    if (!control_transfer(dev, set_cfg, nullptr, 0)) {
        kernel::serial::write("[xhci]: set configuration failed\n");
        return false;
    }

    // Configure bulk endpoints using Configure Endpoint command
    const uint32_t ctx_bytes = g_ctrl.ctx_size;
    const uint32_t max_ctx = 32;

    uint8_t* input_ctx = reinterpret_cast<uint8_t*>(xhci_alloc(ctx_bytes * (max_ctx + 1u), 64));

    if (input_ctx == nullptr) return false;

    zero_mem(input_ctx, ctx_bytes * (max_ctx + 1u));

    uint32_t* add_flags = reinterpret_cast<uint32_t*>(input_ctx + 4);

    const uint8_t out_dci = static_cast<uint8_t>(bulk_out_ep * 2u);
    const uint8_t in_dci = static_cast<uint8_t>(bulk_in_ep * 2u + 1u);
    
    *add_flags = (1u << out_dci) | (1u << in_dci);


    ring_init(&g_dev.bulk_out_ring, kRingSize, true);
    ring_init(&g_dev.bulk_in_ring, kRingSize, true);
    
    if (g_dev.bulk_out_ring.trbs == nullptr || g_dev.bulk_in_ring.trbs == nullptr) return false;

    uint8_t* out_ctx = input_ctx + ctx_bytes * (out_dci + 1u);
    uint8_t* in_ctx = input_ctx + ctx_bytes * (in_dci + 1u);
    
    write_ep_context(out_ctx, reinterpret_cast<uint64_t>(g_dev.bulk_out_ring.trbs), 512, 2u);
    write_ep_context(in_ctx, reinterpret_cast<uint64_t>(g_dev.bulk_in_ring.trbs), 512, 6u);

    Trb* cfg = ring_alloc_trb(&g_ctrl.cmd_ring);
    
    if (cfg == nullptr) return false;
    
    cfg->parameter = reinterpret_cast<uint64_t>(input_ctx);
    cfg->control = (kTrbTypeConfigureEndpoint << 10) | (static_cast<uint32_t>(dev.slot_id) << 24) | (g_ctrl.cmd_ring.cycle ? kTrbCycle : 0);

    uint8_t cc = 0;
    
    if (!submit_command(cfg, nullptr, &cc)) {
        kernel::serial::write("[xhci]: configure endpoints failed\n");
        return false;
    }

    dev.bulk_in.dci = in_dci;
    dev.bulk_in.max_packet = 512;
    dev.bulk_in.in = true;
    
    dev.bulk_out.dci = out_dci;
    dev.bulk_out.max_packet = 512;
    dev.bulk_out.in = false;

    g_dev.dev = dev;
    g_dev.configured = true;

    *out = dev;

    return true;
}

void print_info() {
    (void)init();
}

void handle_irq() {
    if (!g_ctrl.ready) return;

    volatile uint32_t* intr = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(g_ctrl.rt) + 0x20u);

    while (event_ready()) {
        Trb ev = next_event();
        const uint32_t ev_type = (ev.control >> 10) & 0x3Fu;
        const uint8_t code = static_cast<uint8_t>((ev.status >> 24) & 0xFFu);

        // try to record completion in table
        uint64_t ptr = ev.parameter;
        bool recorded = false;

        for (int i = 0; i < 128; ++i) {
            if (g_completions[i].ptr == 0) {
                g_completions[i].ptr = ptr;
                g_completions[i].code = code;
                g_completions[i].ev = ev;
                recorded = true;
                break;
            }
        }

        if (!recorded) {
            kernel::serial::write("[xhci]: completion table full\n");
        }

        // basic logging
        if (ev_type == kTrbTypeTransferEvent) {
            kernel::serial::write("[xhci]: irq transfer event recorded\n");
        } else if (ev_type == kTrbTypeCommandCompletion) {
            kernel::serial::write("[xhci]: irq command completion recorded\n");
        } else {
            kernel::serial::write("[xhci]: irq event type=");
            kernel::serial::write_hex(ev_type);
            kernel::serial::write("\n");
        }
    }

    intr[0x00 / 4] = 1u;
}

// Kernel task entry: poll xHCI events periodically
static void xhci_poll_task_entry() {
    while (true) {
        handle_irq();

#ifdef __x86_64__
        kernel::task::scheduler::sleep_current(1, kernel::arch::x86_64::interrupts::ticks());
#else
        kernel::task::scheduler::sleep_current(1, kernel::arch::x86::interrupts::ticks());
#endif
    }
}

void start_poll_task() {
    // create a small kernel task that polls xHCI
    (void)kernel::task::scheduler::create_kernel_task(xhci_poll_task_entry, 4096);
}

bool wait_for_completion(uint64_t ptr, uint8_t* completion) {
    uint64_t spins = 0;

    for (;;) {
        for (int i = 0; i < 128; ++i) {
            volatile uint64_t p = g_completions[i].ptr;
            if (p == ptr) {
                uint8_t code = g_completions[i].code;
                if (completion) *completion = code;
                // clear
                g_completions[i].ptr = 0;
                g_completions[i].code = 0;
                return code == 1u;
            }
        }

        handle_irq();
        cpu_pause();

        if (++spins >= 50000000u) {
            kernel::serial::write("[xhci]: wait timeout\n");
            return false;
        }
    }

    return false;
}

} // namespace xhci
} // namespace kernel
