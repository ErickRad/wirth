#include "vmm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "heap.hpp"

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kPageEntries = 1024;
constexpr uint32_t kMaxTables = 64;  // 64 * 4 MiB = 256 MiB
constexpr uint32_t kPagePresent = 0x1;
constexpr uint32_t kPageWrite = 0x2;
constexpr uint32_t kPageUser = 0x4;

alignas(4096) uint32_t g_kernel_page_directory[kPageEntries] = {};
alignas(4096) uint32_t g_page_tables[kMaxTables][kPageEntries] = {};

uint16_t g_table_for_dir[kPageEntries] = {};
uint16_t g_tables_in_use = 0;

bool g_paging_enabled = false;

uint32_t* g_current_page_directory = nullptr;

uint16_t invalid_table_index() {
    return static_cast<uint16_t>(0xFFFFu);
}

void clear_page_table(uint32_t* table) {
    for (size_t i = 0; i < kPageEntries; ++i) {
        table[i] = 0;
    }
}

uint32_t* alloc_page_table() {
    if (g_tables_in_use >= kMaxTables) {
        return nullptr;
    }

    uint32_t* const table = &g_page_tables[g_tables_in_use][0];
    clear_page_table(table);

    ++g_tables_in_use;

    return table;
}

void flush_tlb_entry(uint32_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void flush_cr3() {
    uint32_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

}  // namespace

namespace kernel::mm::vmm {

void init() {
    g_tables_in_use = 0;

    for (size_t i = 0; i < kPageEntries; ++i) {
        g_kernel_page_directory[i] = 0;
        g_table_for_dir[i] = invalid_table_index();
    }

    g_current_page_directory = &g_kernel_page_directory[0];

    // Identity-map first 64 MiB so early kernel code/data/devices stay reachable.

    for (uint32_t addr = 0; addr < 64u * 1024u * 1024u; addr += kPageSize) {
        map_page(addr, addr, true, false);
    }
}

bool map_page(uint32_t virt, uint32_t phys, bool write, bool user) {
    const uint32_t dir_index = (virt >> 22) & 0x3FF;
    const uint32_t tbl_index = (virt >> 12) & 0x3FF;

    uint32_t* table = nullptr;

    if (g_table_for_dir[dir_index] == invalid_table_index()) {
        table = alloc_page_table();

        if (table == nullptr) {
            return false;
        }

        const uint16_t table_index = static_cast<uint16_t>(g_tables_in_use - 1);
        g_table_for_dir[dir_index] = table_index;

        uint32_t dir_flags = kPagePresent;

        if (write) {
            dir_flags |= kPageWrite;
        }

        if (user) {
            dir_flags |= kPageUser;
        }

        g_kernel_page_directory[dir_index] = (reinterpret_cast<uint32_t>(table) & 0xFFFFF000u) | dir_flags;

    } else {
        table = &g_page_tables[g_table_for_dir[dir_index]][0];
    }

    uint32_t entry_flags = kPagePresent;

    if (write) {
        entry_flags |= kPageWrite;
    }

    if (user) {
        entry_flags |= kPageUser;
    }

    table[tbl_index] = (phys & 0xFFFFF000u) | entry_flags;

    if (g_paging_enabled) {
        flush_tlb_entry(virt);
    }

    return true;
}

bool unmap_page(uint32_t virt) {
    const uint32_t dir_index = (virt >> 22) & 0x3FF;
    const uint32_t tbl_index = (virt >> 12) & 0x3FF;

    if (g_table_for_dir[dir_index] == invalid_table_index()) {
        return false;
    }

    uint32_t* const table = &g_page_tables[g_table_for_dir[dir_index]][0];

    if ((table[tbl_index] & kPagePresent) == 0) {
        return false;
    }

    table[tbl_index] = 0;

    if (g_paging_enabled) {
        flush_tlb_entry(virt);
    }

    return true;
}

uint32_t virt_to_phys(uint32_t virt) {
    const uint32_t dir_index = (virt >> 22) & 0x3FF;
    const uint32_t tbl_index = (virt >> 12) & 0x3FF;
    const uint32_t page_offset = virt & 0xFFF;

    if (g_table_for_dir[dir_index] == invalid_table_index()) {
        return 0;
    }

    const uint32_t* const table = &g_page_tables[g_table_for_dir[dir_index]][0];
    const uint32_t pte = table[tbl_index];

    if ((pte & kPagePresent) == 0) {
        return 0;
    }

    return (pte & 0xFFFFF000u) | page_offset;
}

void enable_paging() {
    const uint32_t pd_phys = reinterpret_cast<uint32_t>(&g_kernel_page_directory[0]);
    asm volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");

    uint32_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));

    cr0 |= 0x80000000u;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    g_paging_enabled = true;
    g_current_page_directory = &g_kernel_page_directory[0];

    flush_cr3();
}

bool paging_enabled() {
    return g_paging_enabled;
}

AddressSpace kernel_address_space() {
    return &g_kernel_page_directory[0];
}

AddressSpace current_address_space() {
    return g_current_page_directory;
}

AddressSpace create_kernel_clone_address_space() {

    uint32_t* const clone_pd =
        reinterpret_cast<uint32_t*>(kernel::mm::heap::alloc(kPageSize, kPageSize));
    
    if (clone_pd == nullptr) {
        return nullptr;
    }
 
    for (uint32_t i = 0; i < kPageEntries; ++i) {
        clone_pd[i] = g_kernel_page_directory[i];
    }

    return clone_pd;
}

bool switch_address_space(AddressSpace address_space) {
    if (address_space == nullptr) {
        return false;
    }     
    
    const uint32_t pd_phys = virt_to_phys(reinterpret_cast<uint32_t>(address_space));
    
    if (pd_phys == 0) {
        return false;
    }
    
    asm volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
    
    g_current_page_directory = address_space;
    g_paging_enabled = true;
    
    return true;
}

}  // namespace kernel::mm::vmm