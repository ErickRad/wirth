#include "heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "pmm.hpp"
#include "vmm.hpp"

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kHeapHeaderMagic = 0x57485245u;

struct HeapHeader {
    uint32_t block_start;
    uint32_t block_size;
    uint32_t payload_size;
    uint32_t magic;
};

struct FreeBlock {
    uint32_t size;
    FreeBlock* next;
};

uint32_t g_heap_start = 0;
uint32_t g_heap_limit = 0;
uint32_t g_heap_break = 0;
uint32_t g_heap_mapped_end = 0;
FreeBlock* g_free_list = nullptr;
uint32_t g_heap_active_bytes = 0;

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

uint32_t align_up(uint32_t value, uint32_t alignment) {
    if (alignment == 0) {
        alignment = 1;
    }

    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t block_start_of(const FreeBlock* block) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
}

void insert_free_block(uint32_t start, uint32_t size) {
    if (size < sizeof(FreeBlock)) {
        return;
    }

    FreeBlock* node = reinterpret_cast<FreeBlock*>(static_cast<uintptr_t>(start));
    node->size = size;
    node->next = nullptr;

    FreeBlock* prev = nullptr;
    FreeBlock* curr = g_free_list;

    while (curr != nullptr && block_start_of(curr) < start) {
        prev = curr;
        curr = curr->next;
    }

    node->next = curr;

    if (prev == nullptr) {
        g_free_list = node;
    } else {
        prev->next = node;
    }

    if (prev != nullptr) {
        const uint32_t prev_end = block_start_of(prev) + prev->size;

        if (prev_end == start) {
            prev->size += node->size;
            prev->next = node->next;
            node = prev;
        }
    }

    curr = node->next;

    if (curr != nullptr) {
        const uint32_t node_end = block_start_of(node) + node->size;

        if (node_end == block_start_of(curr)) {
            node->size += curr->size;
            node->next = curr->next;
        }
    }
}

void* allocate_from_free_list(size_t size, size_t alignment) {
    FreeBlock* prev = nullptr;
    FreeBlock* curr = g_free_list;

    while (curr != nullptr) {
        const uint32_t start = block_start_of(curr);
        const uint32_t end = start + curr->size;

        uint32_t header = align_up(start + static_cast<uint32_t>(sizeof(HeapHeader)), static_cast<uint32_t>(alignment));

        if (header < start) {
            header = start;
        }

        uint32_t payload = header + static_cast<uint32_t>(sizeof(HeapHeader));
        payload = align_up(payload, static_cast<uint32_t>(alignment));

        if (payload < header + sizeof(HeapHeader)) {
            payload = align_up(start + static_cast<uint32_t>(sizeof(HeapHeader)), static_cast<uint32_t>(alignment));
            header = payload - static_cast<uint32_t>(sizeof(HeapHeader));
        }

        if (header < start) {
            curr = curr->next;
            continue;
        }

        uint32_t alloc_end = payload + static_cast<uint32_t>(size);

        if (alloc_end > end) {
            curr = curr->next;
            continue;
        }

        uint32_t prefix_size = header - start;
        uint32_t suffix_size = end - alloc_end;

        if (prefix_size > 0 && prefix_size < sizeof(FreeBlock)) {
            header = start;
            payload = align_up(header + static_cast<uint32_t>(sizeof(HeapHeader)), static_cast<uint32_t>(alignment));
            alloc_end = payload + static_cast<uint32_t>(size);

            if (alloc_end > end) {
                curr = curr->next;
                continue;
            }

            prefix_size = 0;
            suffix_size = end - alloc_end;
        }

        if (suffix_size > 0 && suffix_size < sizeof(FreeBlock)) {
            alloc_end = end;
            suffix_size = 0;
        }

        if (prev == nullptr) {
            g_free_list = curr->next;
        } else {
            prev->next = curr->next;
        }

        if (prefix_size >= sizeof(FreeBlock)) {
            insert_free_block(start, prefix_size);
        }

        if (suffix_size >= sizeof(FreeBlock)) {
            insert_free_block(alloc_end, suffix_size);
        }

        auto* header_ptr = reinterpret_cast<HeapHeader*>(static_cast<uintptr_t>(header));
        header_ptr->block_start = header;
        header_ptr->block_size = alloc_end - header;
        header_ptr->payload_size = static_cast<uint32_t>(size);
        header_ptr->magic = kHeapHeaderMagic;

        g_heap_active_bytes += static_cast<uint32_t>(size);

        return reinterpret_cast<void*>(static_cast<uintptr_t>(payload));
    }

    return nullptr;
}

void* allocate_from_bump(size_t size, size_t alignment) {
    const uint32_t header_size = static_cast<uint32_t>(sizeof(HeapHeader));
    uint32_t header = align_up(g_heap_break + header_size, static_cast<uint32_t>(alignment));

    if (header < g_heap_break) {
        header = g_heap_break;
    }

    uint32_t payload = align_up(header + header_size, static_cast<uint32_t>(alignment));

    if (payload < header + header_size) {
        payload = align_up(g_heap_break + header_size, static_cast<uint32_t>(alignment));
        header = payload - header_size;
    }

    uint32_t alloc_end = payload + static_cast<uint32_t>(size);

    if (alloc_end < payload || alloc_end > g_heap_limit) {
        return nullptr;
    }

    while (g_heap_mapped_end < alloc_end) {
        if (!map_next_heap_page()) {
            return nullptr;
        }
    }

    g_heap_break = alloc_end;

    auto* header_ptr = reinterpret_cast<HeapHeader*>(static_cast<uintptr_t>(header));
    header_ptr->block_start = header;
    header_ptr->block_size = alloc_end - header;
    header_ptr->payload_size = static_cast<uint32_t>(size);
    header_ptr->magic = kHeapHeaderMagic;

    g_heap_active_bytes += static_cast<uint32_t>(size);

    return reinterpret_cast<void*>(static_cast<uintptr_t>(payload));
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
    g_free_list = nullptr;
    g_heap_active_bytes = 0;
}

void* alloc(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
    
    if (alignment == 0) {
        alignment = 1;
    }
    
    void* ptr = allocate_from_free_list(size, alignment);

    if (ptr != nullptr) {
        return ptr;
    }

    return allocate_from_bump(size, alignment);
}

void free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    auto* header = reinterpret_cast<HeapHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(HeapHeader));

    if (header->magic != kHeapHeaderMagic) {
        return;
    }

    if (g_heap_active_bytes >= header->payload_size) {
        g_heap_active_bytes -= header->payload_size;
    } else {
        g_heap_active_bytes = 0;
    }

    header->magic = 0;
    insert_free_block(header->block_start, header->block_size);
}

uint32_t mapped_bytes() {
    return g_heap_mapped_end - g_heap_start;
}

uint32_t used_bytes() {
    return g_heap_active_bytes;
}

}  // namespace kernel::mm::heap
