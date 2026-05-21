#include "nvme.hpp"

#include "block.hpp"
#include "pci.hpp"
#include "serial.hpp"

#include <stddef.h>
#include <stdint.h>

#ifndef __x86_64__
#include "mm/heap.hpp"
#include "mm/vmm.hpp"
#endif

namespace {

constexpr uint32_t kPageSize = 4096u;
constexpr uint32_t kQueueDepth = 32u;
constexpr uint32_t kPrpListEntries = 512u;
constexpr uint32_t kNvmeHeapSize = 256u * 1024u;
constexpr uint32_t kAdminQueueId = 0u;
constexpr uint32_t kIoQueueId = 1u;

alignas(4096) static uint8_t g_nvme_heap[kNvmeHeapSize] = {};
alignas(4096) static uint64_t g_prp_list[kPrpListEntries] = {};
static size_t g_nvme_heap_used = 0;

struct alignas(8) NvmeCommand {
    uint8_t opcode;
    uint8_t fuse;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct alignas(16) NvmeCompletion {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
} __attribute__((packed));

struct NvmeQueue {
    NvmeCommand* sq;
    NvmeCompletion* cq;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    bool cq_phase;
    uint16_t next_cid;
};

static volatile uint32_t* g_regs = nullptr;
static volatile uint32_t* g_doorbells = nullptr;
static uint32_t g_doorbell_stride = 1u;
static uint64_t g_cap = 0;
static uint32_t g_block_size = 512u;
static uint64_t g_block_count = 0;
static uint32_t g_nsid = 1u;
static bool g_ready = false;
static NvmeQueue g_admin = {};
static NvmeQueue g_io = {};

static bool nvme_read_sectors(uint64_t lba, uint8_t* buf, uint32_t count);
static bool nvme_write_sectors(uint64_t lba, const uint8_t* buf, uint32_t count);

static void zero_mem(void* ptr, size_t size) {
    uint8_t* out = reinterpret_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        out[i] = 0;
    }
}

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1u);
}

[[maybe_unused]] static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void cpu_pause() {
    asm volatile("pause" ::: "memory");
}

static void* nvme_alloc(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
    if (alignment == 0) alignment = 1;

    const size_t mask = alignment - 1u;
    const size_t aligned = (g_nvme_heap_used + mask) & ~mask;

    if (aligned + size > sizeof(g_nvme_heap)) return nullptr;

    g_nvme_heap_used = aligned + size;
    return g_nvme_heap + aligned;
}

#ifdef __x86_64__
extern "C" uint64_t pml4[];

static uint64_t* g_mmio_pdpt = nullptr;
static uint64_t* g_mmio_pd = nullptr;
static uint64_t g_mmio_next = 0xFFFF800000000000ull;

static void reload_cr3() {
    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static bool ensure_mmio_tables() {
    const uint64_t pml4_index = (g_mmio_next >> 39) & 0x1FFu;

    if ((pml4[pml4_index] & 1u) == 0u) {
        g_mmio_pdpt = reinterpret_cast<uint64_t*>(nvme_alloc(kPageSize, kPageSize));
        if (g_mmio_pdpt == nullptr) return false;

        zero_mem(g_mmio_pdpt, kPageSize);
        pml4[pml4_index] = (reinterpret_cast<uint64_t>(g_mmio_pdpt) & ~0xFFFull) | 0x3u;
    } else {
        g_mmio_pdpt = reinterpret_cast<uint64_t*>(pml4[pml4_index] & ~0xFFFull);
    }

    if (g_mmio_pd == nullptr) {
        g_mmio_pd = reinterpret_cast<uint64_t*>(nvme_alloc(kPageSize, kPageSize));
        if (g_mmio_pd == nullptr) return false;

        zero_mem(g_mmio_pd, kPageSize);
        g_mmio_pdpt[0] = (reinterpret_cast<uint64_t>(g_mmio_pd) & ~0xFFFull) | 0x3u;
    }

    return true;
}

static void* map_physical_region(uint64_t phys, uint32_t size) {
    if (!ensure_mmio_tables()) return nullptr;

    const uint64_t phys_base = align_down(phys, 0x200000ull);
    const uint64_t offset = phys - phys_base;
    const uint64_t map_size = static_cast<uint64_t>(size) + offset;
    const uint64_t pages = (map_size + 0x200000ull - 1u) / 0x200000ull;

    const uint64_t vbase = align_up(g_mmio_next, 0x200000ull);
    g_mmio_next = vbase + pages * 0x200000ull;

    const uint64_t pd_base_index = (vbase >> 21) & 0x1FFu;
    if (pd_base_index + pages > 512u) return nullptr;

    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t vaddr = vbase + i * 0x200000ull;
        const uint64_t paddr = phys_base + i * 0x200000ull;

        g_mmio_pd[pd_base_index + i] = (paddr & ~0x1FFFFFull) | 0x183u;
        asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(vaddr)) : "memory");
    }

    reload_cr3();
    return reinterpret_cast<void*>(vbase + offset);
}
#else
static void* map_physical_region(uint64_t phys, uint32_t size) {
    const uint32_t page = kPageSize;
    const uint32_t pages = (size + page - 1u) / page;

    void* virt = kernel::mm::heap::alloc(static_cast<size_t>(pages) * page, page);
    if (virt == nullptr) return nullptr;

    uint32_t virt_base = reinterpret_cast<uint32_t>(virt);
    uint32_t phys_base = static_cast<uint32_t>(align_down(phys, page));

    for (uint32_t i = 0; i < pages; ++i) {
        if (!kernel::mm::vmm::map_page(virt_base + i * page, phys_base + i * page, true, false)) {
            return nullptr;
        }
    }

    return virt;
}
#endif

static uint64_t phys_of(const void* ptr) {
#ifdef __x86_64__
    return reinterpret_cast<uint64_t>(ptr);
#else
    return kernel::mm::vmm::virt_to_phys(reinterpret_cast<uint32_t>(ptr));
#endif
}

static void ring_doorbell(uint32_t queue_id, bool completion, uint32_t value) {
    const uint32_t doorbell_index = (queue_id * 2u + (completion ? 1u : 0u)) * g_doorbell_stride;
    g_doorbells[doorbell_index] = value;
}

static void queue_init(NvmeQueue* queue, uint32_t depth) {
    queue->sq = reinterpret_cast<NvmeCommand*>(nvme_alloc(static_cast<size_t>(depth) * sizeof(NvmeCommand), kPageSize));
    queue->cq = reinterpret_cast<NvmeCompletion*>(nvme_alloc(static_cast<size_t>(depth) * sizeof(NvmeCompletion), kPageSize));
    queue->depth = static_cast<uint16_t>(depth);
    queue->sq_tail = 0;
    queue->cq_head = 0;
    queue->cq_phase = true;
    queue->next_cid = 1;

    if (queue->sq != nullptr) zero_mem(queue->sq, static_cast<size_t>(depth) * sizeof(NvmeCommand));
    if (queue->cq != nullptr) zero_mem(queue->cq, static_cast<size_t>(depth) * sizeof(NvmeCompletion));
}

static bool build_prps(void* buffer, uint32_t bytes, uint64_t* prp1, uint64_t* prp2) {
    if (buffer == nullptr || prp1 == nullptr || prp2 == nullptr) return false;

    *prp1 = 0;
    *prp2 = 0;

    if (bytes == 0) return true;

    const uint64_t first_phys = phys_of(buffer);
    const uint32_t first_offset = static_cast<uint32_t>(first_phys & (kPageSize - 1u));
    const uint32_t first_span = kPageSize - first_offset;

    *prp1 = first_phys;

    if (bytes <= first_span) {
        return true;
    }

    uint32_t remaining = bytes - first_span;
    uint8_t* cursor = reinterpret_cast<uint8_t*>(buffer) + first_span;

    if (remaining <= kPageSize) {
        *prp2 = phys_of(cursor);
        return true;
    }

    uint32_t entry = 0;
    while (remaining > 0 && entry < kPrpListEntries) {
        g_prp_list[entry++] = phys_of(cursor);
        const uint32_t chunk = remaining > kPageSize ? kPageSize : remaining;
        cursor += chunk;
        remaining -= chunk;
    }

    if (remaining != 0) {
        return false;
    }

    *prp2 = phys_of(g_prp_list);
    return true;
}

static bool wait_for_cqe(NvmeQueue* queue, uint32_t queue_id, uint16_t cid, uint8_t* completion) {
    for (;;) {
        const NvmeCompletion& cqe = queue->cq[queue->cq_head];
        const uint16_t cqe_cid = static_cast<uint16_t>(cqe.dw3 & 0xFFFFu);
        const uint16_t cqe_status = static_cast<uint16_t>(cqe.dw3 >> 16);
        const bool phase = (cqe_status & 1u) != 0u;

        if (phase != queue->cq_phase || cqe_cid != cid) {
            cpu_pause();
            continue;
        }

        const uint8_t status_code = static_cast<uint8_t>((cqe_status >> 1) & 0x7Fu);
        if (completion != nullptr) {
            *completion = status_code;
        }

        queue->cq[queue->cq_head] = {};
        queue->cq_head = static_cast<uint16_t>((queue->cq_head + 1u) % queue->depth);
        if (queue->cq_head == 0u) {
            queue->cq_phase = !queue->cq_phase;
        }

        ring_doorbell(queue_id, true, queue->cq_head);
        return status_code == 0u;
    }
}

static bool submit_command(NvmeQueue* queue, uint32_t queue_id, NvmeCommand* command, uint8_t* completion) {
    if (queue == nullptr || queue->sq == nullptr || queue->cq == nullptr || command == nullptr) {
        return false;
    }

    const uint16_t cid = queue->next_cid == 0u ? 1u : queue->next_cid;
    queue->next_cid = static_cast<uint16_t>(cid + 1u);

    command->cid = cid;

    queue->sq[queue->sq_tail] = *command;
    queue->sq_tail = static_cast<uint16_t>((queue->sq_tail + 1u) % queue->depth);

    ring_doorbell(queue_id, false, queue->sq_tail);
    return wait_for_cqe(queue, queue_id, cid, completion);
}

[[maybe_unused]] static bool init_namespace(uint32_t namespace_id, uint32_t depth) {
    queue_init(&g_admin, depth);
    queue_init(&g_io, depth);

    if (g_admin.sq == nullptr || g_admin.cq == nullptr || g_io.sq == nullptr || g_io.cq == nullptr) {
        return false;
    }

    const uint64_t admin_sq_phys = phys_of(g_admin.sq);
    const uint64_t admin_cq_phys = phys_of(g_admin.cq);

    g_regs[0x24 / 4] = ((depth - 1u) << 16) | (depth - 1u);
    g_regs[0x28 / 4] = static_cast<uint32_t>(admin_sq_phys);
    g_regs[0x2C / 4] = static_cast<uint32_t>(admin_sq_phys >> 32);
    g_regs[0x30 / 4] = static_cast<uint32_t>(admin_cq_phys);
    g_regs[0x34 / 4] = static_cast<uint32_t>(admin_cq_phys >> 32);

    g_regs[0x14 / 4] = (6u << 16) | (4u << 20) | 1u;

    for (uint32_t spins = 0; spins < 2000000u; ++spins) {
        if ((g_regs[0x1C / 4] & 1u) != 0u) {
            cpu_pause();
            continue;
        }
        break;
    }

    NvmeCommand identify = {};
    identify.opcode = 0x06u;
    identify.nsid = namespace_id;
    identify.cdw10 = 0u;

    uint8_t* identify_buf = reinterpret_cast<uint8_t*>(nvme_alloc(kPageSize, kPageSize));
    if (identify_buf == nullptr) {
        return false;
    }

    zero_mem(identify_buf, kPageSize);

    if (!build_prps(identify_buf, kPageSize, &identify.prp1, &identify.prp2)) {
        return false;
    }

    if (!submit_command(&g_admin, kAdminQueueId, &identify, nullptr)) {
        return false;
    }

    const uint8_t* idb = identify_buf;
    uint64_t nsze = 0;
    for (int i = 0; i < 8; ++i) {
        nsze |= static_cast<uint64_t>(idb[i]) << (i * 8);
    }

    const uint8_t flbas = idb[26];
    const uint8_t lbaf = static_cast<uint8_t>(flbas & 0x0Fu);
    const uint32_t lbaf_offset = 128u + static_cast<uint32_t>(lbaf) * 4u;
    const uint32_t lbaf_dw0 = *reinterpret_cast<const uint32_t*>(idb + lbaf_offset);
    const uint32_t lbads = lbaf_dw0 & 0xFFu;
    const uint32_t lba_size = 1u << lbads;

    NvmeCommand create_cq = {};
    create_cq.opcode = 0x05u;
    create_cq.prp1 = phys_of(g_io.cq);
    create_cq.cdw10 = (kIoQueueId & 0xFFFFu) | ((depth - 1u) << 16);
    create_cq.cdw11 = 1u;

    if (!submit_command(&g_admin, kAdminQueueId, &create_cq, nullptr)) {
        kernel::serial::write("nvme: create cq failed\n");
        return false;
    }

    NvmeCommand create_sq = {};
    create_sq.opcode = 0x01u;
    create_sq.prp1 = phys_of(g_io.sq);
    create_sq.cdw10 = (kIoQueueId & 0xFFFFu) | ((depth - 1u) << 16);
    create_sq.cdw11 = (kIoQueueId << 16) | 1u;

    if (!submit_command(&g_admin, kAdminQueueId, &create_sq, nullptr)) {
        kernel::serial::write("nvme: create sq failed\n");
        return false;
    }

    g_block_size = lba_size;
    g_block_count = nsze;
    g_nsid = namespace_id;
    g_ready = true;

    static kernel::BlockDevice nvme_block = {};
    nvme_block.name = "nvme0";
    nvme_block.block_size = g_block_size;
    nvme_block.block_count = g_block_count;
    nvme_block.read_sectors = &nvme_read_sectors;
    nvme_block.write_sectors = &nvme_write_sectors;
    nvme_block.ctx = nullptr;

    (void)kernel::block_register_device(&nvme_block);

    kernel::serial::write("nvme: controller ready\n");
    return true;
}

static bool submit_io_transfer(uint8_t opcode, uint64_t lba, void* buffer, uint32_t count) {
    if (!g_ready || g_io.sq == nullptr || g_io.cq == nullptr || buffer == nullptr || count == 0u) {
        return false;
    }

    const uint64_t bytes = static_cast<uint64_t>(count) * g_block_size;
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;

    if (!build_prps(buffer, static_cast<uint32_t>(bytes), &prp1, &prp2)) {
        return false;
    }

    NvmeCommand cmd = {};
    cmd.opcode = opcode;
    cmd.nsid = g_nsid;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = static_cast<uint32_t>(lba & 0xFFFFFFFFu);
    cmd.cdw11 = static_cast<uint32_t>(lba >> 32);
    cmd.cdw12 = count - 1u;

    return submit_command(&g_io, kIoQueueId, &cmd, nullptr);
}

static bool nvme_read_sectors(uint64_t lba, uint8_t* buf, uint32_t count) {
    return submit_io_transfer(0x02u, lba, buf, count);
}

static bool nvme_write_sectors(uint64_t lba, const uint8_t* buf, uint32_t count) {
    return submit_io_transfer(0x01u, lba, const_cast<uint8_t*>(buf), count);
}

static uint64_t read_mmio_bar(const kernel::pci::Device& dev) {
    for (int bar = 0; bar < 6; ++bar) {
        const uint32_t value = dev.bar[bar];

        if (value == 0u || (value & 1u) != 0u) {
            continue;
        }

        if ((value & 0x6u) == 0x4u && bar + 1 < 6) {
            const uint64_t low = static_cast<uint64_t>(value & ~0xFu);
            const uint64_t high = static_cast<uint64_t>(dev.bar[bar + 1]);
            return (high << 32) | low;
        }

        return static_cast<uint64_t>(value & ~0xFu);
    }

    return 0u;
}

} // namespace

namespace kernel {
namespace nvme {

bool init() {
    if (g_ready) {
        return true;
    }

    kernel::pci::Device devs[64];
    uint32_t found = 0;

    if (!kernel::pci::scan_devices(devs, 64, &found) || found == 0u) {
        return false;
    }

    for (uint32_t i = 0; i < found; ++i) {
        const kernel::pci::Device& dev = devs[i];

        if (dev.class_code != 0x01u || dev.subclass != 0x08u) {
            continue;
        }

        const uint64_t mmio = read_mmio_bar(dev);
        if (mmio == 0u) {
            continue;
        }

        void* base = map_physical_region(mmio, 0x1000u);
        if (base == nullptr) {
            kernel::serial::write("nvme: MMIO map failed\n");
            continue;
        }

        g_regs = reinterpret_cast<volatile uint32_t*>(base);
        g_cap = *reinterpret_cast<volatile uint64_t*>(g_regs);
        g_doorbell_stride = 1u << static_cast<uint32_t>((g_cap >> 32) & 0xFu);
        g_doorbells = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(base) + 0x1000u);

        g_nvme_heap_used = 0;
        g_ready = false;

        const uint32_t mqes = static_cast<uint32_t>(g_cap & 0xFFFFu);
        uint32_t depth = mqes + 1u;
        if (depth > kQueueDepth) depth = kQueueDepth;
        if (depth < 2u) depth = 2u;

        queue_init(&g_admin, depth);
        queue_init(&g_io, depth);

        if (g_admin.sq == nullptr || g_admin.cq == nullptr || g_io.sq == nullptr || g_io.cq == nullptr) {
            kernel::serial::write("nvme: queue allocation failed\n");
            continue;
        }

        const uint64_t admin_sq_phys = phys_of(g_admin.sq);
        const uint64_t admin_cq_phys = phys_of(g_admin.cq);

        g_regs[0x24 / 4] = ((depth - 1u) << 16) | (depth - 1u);
        g_regs[0x28 / 4] = static_cast<uint32_t>(admin_sq_phys);
        g_regs[0x2C / 4] = static_cast<uint32_t>(admin_sq_phys >> 32);
        g_regs[0x30 / 4] = static_cast<uint32_t>(admin_cq_phys);
        g_regs[0x34 / 4] = static_cast<uint32_t>(admin_cq_phys >> 32);

        g_regs[0x14 / 4] = (6u << 16) | (4u << 20) | 1u;

        for (uint32_t spins = 0; spins < 2000000u; ++spins) {
            if ((g_regs[0x1C / 4] & 1u) != 0u) {
                cpu_pause();
                continue;
            }
            break;
        }

        NvmeCommand identify = {};
        identify.opcode = 0x06u;
        identify.nsid = 1u;

        uint8_t* identify_buf = reinterpret_cast<uint8_t*>(nvme_alloc(kPageSize, kPageSize));
        if (identify_buf == nullptr) {
            continue;
        }

        zero_mem(identify_buf, kPageSize);
        if (!build_prps(identify_buf, kPageSize, &identify.prp1, &identify.prp2)) {
            continue;
        }

        if (!submit_command(&g_admin, kAdminQueueId, &identify, nullptr)) {
            kernel::serial::write("nvme: identify failed\n");
            continue;
        }

        const uint8_t* idb = identify_buf;
        uint64_t nsze = 0;
        for (int k = 0; k < 8; ++k) {
            nsze |= static_cast<uint64_t>(idb[k]) << (k * 8);
        }

        const uint8_t flbas = idb[26];
        const uint8_t lbaf = static_cast<uint8_t>(flbas & 0x0Fu);
        const uint32_t lbaf_offset = 128u + static_cast<uint32_t>(lbaf) * 4u;
        const uint32_t lbaf_dw0 = *reinterpret_cast<const uint32_t*>(idb + lbaf_offset);
        const uint32_t lbads = lbaf_dw0 & 0xFFu;
        const uint32_t lba_size = 1u << lbads;

        NvmeCommand create_cq = {};
        create_cq.opcode = 0x05u;
        create_cq.prp1 = phys_of(g_io.cq);
        create_cq.cdw10 = (kIoQueueId & 0xFFFFu) | ((depth - 1u) << 16);
        create_cq.cdw11 = 1u;

        if (!submit_command(&g_admin, kAdminQueueId, &create_cq, nullptr)) {
            kernel::serial::write("nvme: create cq failed\n");
            continue;
        }

        NvmeCommand create_sq = {};
        create_sq.opcode = 0x01u;
        create_sq.prp1 = phys_of(g_io.sq);
        create_sq.cdw10 = (kIoQueueId & 0xFFFFu) | ((depth - 1u) << 16);
        create_sq.cdw11 = (kIoQueueId << 16) | 1u;

        if (!submit_command(&g_admin, kAdminQueueId, &create_sq, nullptr)) {
            kernel::serial::write("nvme: create sq failed\n");
            continue;
        }

        g_block_size = lba_size;
        g_block_count = nsze;
        g_nsid = 1u;
        g_ready = true;

        static kernel::BlockDevice nvme_block = {};
        nvme_block.name = "nvme0";
        nvme_block.block_size = g_block_size;
        nvme_block.block_count = g_block_count;
        nvme_block.read_sectors = &nvme_read_sectors;
        nvme_block.write_sectors = &nvme_write_sectors;
        nvme_block.ctx = nullptr;

        (void)kernel::block_register_device(&nvme_block);

        kernel::serial::write("nvme: controller ready\n");
        return true;
    }

    return false;
}

} // namespace nvme
} // namespace kernel
