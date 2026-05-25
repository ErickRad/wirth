#pragma once

#include <stdint.h>

namespace kernel::boot::acpi {

struct AcpiInfo {
    bool found;
    bool checksum_ok;
    bool has_xsdt;
    uint64_t rsdp_phys;
    uint64_t rsdt_phys;
    uint64_t xsdt_phys;
    uint64_t madt_phys;
    uint32_t local_apic_phys;
    uint32_t ioapic_count;
    // Support up to 8 IOAPIC entries discovered in MADT
    uint32_t ioapic_phys[8];
    uint32_t ioapic_gsi_base[8];
};

bool discover(uint32_t multiboot_info_addr, AcpiInfo* out_info);
void log(uint32_t multiboot_info_addr);

}  // namespace kernel::boot::acpi