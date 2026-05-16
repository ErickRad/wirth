#pragma once

#include <stdint.h>

namespace kernel::boot::multiboot2 {

struct MemoryMapEntryView {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
};

using MemoryMapVisitor = void (*)(const MemoryMapEntryView& entry, void* user_data);

bool visit_memory_map(uint32_t multiboot_info_addr, MemoryMapVisitor visitor, void* user_data);
void log_memory_map(uint32_t multiboot_info_addr);

}  // namespace kernel::boot::multiboot2
