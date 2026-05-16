#include "multiboot2.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../serial.hpp"

namespace {

struct [[gnu::packed]] TagHeader {
    uint32_t type;
    uint32_t size;
};

struct [[gnu::packed]] MemoryMapTag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct [[gnu::packed]] MemoryMapEntry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

constexpr uint32_t kTagTypeEnd = 0;
constexpr uint32_t kTagTypeMemoryMap = 6;
constexpr uint32_t kMemoryTypeAvailable = 1;
constexpr size_t kLogRegionLimit = 12;

uintptr_t align8(uintptr_t value) {
    return (value + 7u) & ~static_cast<uintptr_t>(7u);
}

void log_entry(const MemoryMapEntry& entry) {
    kernel::serial::write("[kernigham] mmap base=0x");
    kernel::serial::write_hex64(entry.base_addr);
    kernel::serial::write(" len=0x");
    kernel::serial::write_hex64(entry.length);
    kernel::serial::write(" type=0x");
    kernel::serial::write_hex(entry.type);
    if (entry.type == kMemoryTypeAvailable) {
        kernel::serial::write(" (available)");
    }
    kernel::serial::write("\n");
}

}  // namespace

namespace kernel::boot::multiboot2 {

bool visit_memory_map(uint32_t multiboot_info_addr, MemoryMapVisitor visitor, void* user_data) {
    if (visitor == nullptr) {
        return false;
    }
    const uint8_t* const info = reinterpret_cast<const uint8_t*>(multiboot_info_addr);
    if (info == nullptr) {
        return false;
    }

    const uint32_t total_size = *reinterpret_cast<const uint32_t*>(info);
    const uintptr_t info_begin = reinterpret_cast<uintptr_t>(info);
    const uintptr_t info_end = info_begin + total_size;

    uintptr_t cursor = info_begin + 8;
    bool found = false;

    while (cursor + sizeof(TagHeader) <= info_end) {
        const auto* tag = reinterpret_cast<const TagHeader*>(cursor);
        if (tag->type == kTagTypeEnd) {
            break;
        }

        if (tag->type == kTagTypeMemoryMap && tag->size >= sizeof(MemoryMapTag)) {
            found = true;
            const auto* mmap_tag = reinterpret_cast<const MemoryMapTag*>(cursor);
            const uintptr_t entries_begin = cursor + sizeof(MemoryMapTag);
            const uintptr_t entries_end = cursor + mmap_tag->size;
            uintptr_t entry_cursor = entries_begin;

            while (entry_cursor + mmap_tag->entry_size <= entries_end && mmap_tag->entry_size >= sizeof(MemoryMapEntry)) {
                const auto* entry = reinterpret_cast<const MemoryMapEntry*>(entry_cursor);
                const MemoryMapEntryView view = {
                    .base_addr = entry->base_addr,
                    .length = entry->length,
                    .type = entry->type,
                };
                visitor(view, user_data);
                entry_cursor += mmap_tag->entry_size;
            }
        }

        cursor = align8(cursor + tag->size);
    }

    return found;
}

namespace {

struct LogContext {
    size_t logged_entries;
};

void log_visitor(const MemoryMapEntryView& entry, void* user_data) {
    auto* ctx = reinterpret_cast<LogContext*>(user_data);
    if (ctx->logged_entries >= kLogRegionLimit) {
        return;
    }
    const MemoryMapEntry raw = {
        .base_addr = entry.base_addr,
        .length = entry.length,
        .type = entry.type,
        .reserved = 0,
    };
    log_entry(raw);
    ++ctx->logged_entries;
}

}  // namespace

void log_memory_map(uint32_t multiboot_info_addr) {
    LogContext ctx = {.logged_entries = 0};
    const bool found = visit_memory_map(multiboot_info_addr, log_visitor, &ctx);
    if (!found) {
        kernel::serial::write("[kernigham] mmap tag not found\n");
    } else if (ctx.logged_entries == 0) {
        kernel::serial::write("[kernigham] mmap parsed without entries\n");
    }
}

}  // namespace kernel::boot::multiboot2
