#pragma once

#include <stdint.h>

namespace kernel::boot::multiboot2 {

struct MemoryMapEntryView {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
};

struct ModuleView {
    uint32_t start_addr;
    uint32_t end_addr;
    const char* string;
};

using MemoryMapVisitor = void (*)(const MemoryMapEntryView& entry, void* user_data);
using ModuleVisitor = void (*)(const ModuleView& module, void* user_data);

bool visit_memory_map(uint32_t multiboot_info_addr, MemoryMapVisitor visitor, void* user_data);
void log_memory_map(uint32_t multiboot_info_addr);
bool visit_modules(uint32_t multiboot_info_addr, ModuleVisitor visitor, void* user_data);
void log_modules(uint32_t multiboot_info_addr);

}  // namespace kernel::boot::multiboot2
