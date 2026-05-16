#include "pmm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../boot/multiboot2.hpp"

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMaxFrames = 1024 * 1024;  // up to 4 GiB with 4 KiB pages
constexpr uint32_t kBitmapSize = kMaxFrames / 32;
constexpr uint32_t kMemoryTypeAvailable = 1;
constexpr uint32_t kLowMemoryReserveEnd = 0x00100000;  // keep first 1 MiB reserved

uint32_t g_bitmap[kBitmapSize] = {};
uint32_t g_total_frames = 0;
uint32_t g_free_frames = 0;

uint32_t align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1);
}

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool frame_index_valid(uint32_t frame) {
    return frame < kMaxFrames;
}

bool is_set(uint32_t frame) {
    const uint32_t word = frame / 32;
    const uint32_t bit = frame % 32;
    return (g_bitmap[word] & (1u << bit)) != 0;
}

void set_bit(uint32_t frame) {
    if (!frame_index_valid(frame)) {
        return;
    }
    const uint32_t word = frame / 32;
    const uint32_t bit = frame % 32;
    if (!is_set(frame)) {
        g_bitmap[word] |= (1u << bit);
        if (g_free_frames > 0) {
            --g_free_frames;
        }
    }
}

void clear_bit(uint32_t frame) {
    if (!frame_index_valid(frame)) {
        return;
    }
    const uint32_t word = frame / 32;
    const uint32_t bit = frame % 32;
    if (is_set(frame)) {
        g_bitmap[word] &= ~(1u << bit);
        ++g_free_frames;
    }
}

void reserve_range(uint32_t start_addr, uint32_t end_addr) {
    const uint64_t start = align_down(start_addr, kPageSize);
    uint64_t end = align_up(end_addr, kPageSize);
    const uint64_t max_phys = static_cast<uint64_t>(kMaxFrames) * kPageSize;
    if (end > max_phys) {
        end = max_phys;
    }
    for (uint64_t addr = start; addr < end; addr += kPageSize) {
        set_bit(static_cast<uint32_t>(addr / kPageSize));
    }
}

void unreserve_range(uint32_t start_addr, uint32_t end_addr) {
    const uint64_t start = align_down(start_addr, kPageSize);
    uint64_t end = align_up(end_addr, kPageSize);
    const uint64_t max_phys = static_cast<uint64_t>(kMaxFrames) * kPageSize;
    if (end > max_phys) {
        end = max_phys;
    }
    for (uint64_t addr = start; addr < end; addr += kPageSize) {
        clear_bit(static_cast<uint32_t>(addr / kPageSize));
    }
}

struct InitContext {};

void map_visitor(const kernel::boot::multiboot2::MemoryMapEntryView& entry, void* user_data) {
    (void)user_data;
    if (entry.type != kMemoryTypeAvailable || entry.length == 0) {
        return;
    }

    const uint64_t end64 = entry.base_addr + entry.length;
    if (entry.base_addr >= 0x100000000ULL) {
        return;
    }

    uint32_t start = static_cast<uint32_t>(entry.base_addr);
    uint32_t end = (end64 >= 0x100000000ULL) ? 0xFFFFF000u : static_cast<uint32_t>(end64);
    if (end <= start) {
        return;
    }
    unreserve_range(start, end);
}

}  // namespace

namespace kernel::mm::pmm {

void init(uint32_t multiboot_info_addr, uint32_t kernel_start, uint32_t kernel_end) {
    for (uint32_t i = 0; i < kBitmapSize; ++i) {
        g_bitmap[i] = 0xFFFFFFFFu;
    }
    g_total_frames = kMaxFrames;
    g_free_frames = 0;

    InitContext ctx = {};
    (void)ctx;
    kernel::boot::multiboot2::visit_memory_map(multiboot_info_addr, map_visitor, &ctx);
    reserve_range(0, kLowMemoryReserveEnd);
    reserve_range(kernel_start, kernel_end);
}

uint32_t alloc_frame() {
    if (g_free_frames == 0) {
        return 0;
    }
    for (uint32_t word = 0; word < kBitmapSize; ++word) {
        if (g_bitmap[word] == 0xFFFFFFFFu) {
            continue;
        }
        for (uint32_t bit = 0; bit < 32; ++bit) {
            const uint32_t frame = word * 32 + bit;
            if (!frame_index_valid(frame)) {
                return 0;
            }
            if (!is_set(frame)) {
                set_bit(frame);
                return frame * kPageSize;
            }
        }
    }
    return 0;
}

void free_frame(uint32_t phys_addr) {
    if ((phys_addr % kPageSize) != 0) {
        return;
    }
    clear_bit(phys_addr / kPageSize);
}

uint32_t total_frames() {
    return g_total_frames;
}

uint32_t free_frames() {
    return g_free_frames;
}

}  // namespace kernel::mm::pmm
