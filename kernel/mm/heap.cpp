#include "heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "pmm.hpp"
#include "vmm.hpp"

namespace {

constexpr uint32_t kPageSize = 4096;

uint32_t g_heap_start = 0;
uint32_t g_heap_limit = 0;
uint32_t g_heap_break = 0;
uint32_t g_heap_mapped_end = 0;

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool map_next_heap_page() {
    if (g_heap_mapped_end >= g_heap_limit) {
        return false;
    }    
    
    const uint32_t frame = kernel::mm::pmm::alloc_frame();
    
    if (frame == 0) {
        return false;
    }
    
    if (!kernel::mm::vmm::map_page(g_heap_mapped_end, frame, true, false)) {
        kernel::mm::pmm::free_frame(frame);
        return false;
    }
    
    g_heap_mapped_end += kPageSize;
    
    return true;
}

}  // namespace

namespace kernel::mm::heap {

void init(uint32_t heap_start, uint32_t heap_limit) {
    g_heap_start = align_up(heap_start, kPageSize);
    g_heap_limit = align_up(heap_limit, kPageSize);
    
    if (g_heap_limit <= g_heap_start) {
        g_heap_limit = g_heap_start;
    }
    
    g_heap_break = g_heap_start;
    g_heap_mapped_end = g_heap_start;
}

void* alloc(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
    
    if (alignment == 0) {
        alignment = 1;
    }
    
    const uint32_t aligned_break = align_up(g_heap_break, static_cast<uint32_t>(alignment));
    const uint32_t required_end = aligned_break + static_cast<uint32_t>(size);
    
    if (required_end < aligned_break || required_end > g_heap_limit) {
        return nullptr;
    }

    while (g_heap_mapped_end < required_end) {
        if (!map_next_heap_page()) {
            return nullptr;
        }
    }

    g_heap_break = required_end;
    
    return reinterpret_cast<void*>(aligned_break);
}

uint32_t mapped_bytes() {
    return g_heap_mapped_end - g_heap_start;
}

uint32_t used_bytes() {
    return g_heap_break - g_heap_start;
}

}  // namespace kernel::mm::heap
