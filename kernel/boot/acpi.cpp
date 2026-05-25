#include "acpi.hpp"

#include <stddef.h>

#include "../serial.hpp"

namespace {

struct [[gnu::packed]] TagHeader {
    uint32_t type;
    uint32_t size;
};

struct [[gnu::packed]] AcpiOldTag {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[20];
};

struct [[gnu::packed]] AcpiNewTag {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[36];
};

struct [[gnu::packed]] RsdpV1 {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
};

struct [[gnu::packed]] RsdpV2 {
    RsdpV1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] SdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] MadtHeader {
    SdtHeader header;
    uint32_t local_apic_address;
    uint32_t flags;
};

struct [[gnu::packed]] MadtEntryHeader {
    uint8_t type;
    uint8_t length;
};

struct [[gnu::packed]] MadtIoApicEntry {
    uint8_t type;
    uint8_t length;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
};

constexpr uint32_t kTagTypeEnd = 0;
constexpr uint32_t kTagTypeAcpiOld = 14;
constexpr uint32_t kTagTypeAcpiNew = 15;
constexpr uint32_t kMadtSignature = 0x50434941u;  // 'APIC'

uintptr_t align8(uintptr_t value) {
    return (value + 7u) & ~static_cast<uintptr_t>(7u);
}

bool text_equal4(const char* a, const char* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

uint8_t checksum8(const uint8_t* data, uint32_t len) {
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; ++i) {
        sum = static_cast<uint8_t>(sum + data[i]);
    }

    return sum;
}

bool parse_rsdp(const uint8_t* rsdp_bytes, uint32_t size, kernel::boot::acpi::AcpiInfo& info) {
    if (rsdp_bytes == nullptr || size < sizeof(RsdpV1)) {
        return false;
    }

    const auto* v1 = reinterpret_cast<const RsdpV1*>(rsdp_bytes);

    if (!text_equal4(v1->signature, "RSD ") || v1->signature[4] != 'P' || v1->signature[5] != 'T' || v1->signature[6] != 'R' || v1->signature[7] != ' ') {
        return false;
    }

    info.found = true;
    info.rsdp_phys = reinterpret_cast<uintptr_t>(rsdp_bytes);
    info.rsdt_phys = v1->rsdt_address;
    info.has_xsdt = false;
    info.xsdt_phys = 0;
    info.madt_phys = 0;
    info.local_apic_phys = 0;
    info.ioapic_count = 0;

    const uint32_t v1_len = sizeof(RsdpV1);
    uint8_t sum = checksum8(rsdp_bytes, v1_len);

    if (size >= sizeof(RsdpV2)) {
        const auto* v2 = reinterpret_cast<const RsdpV2*>(rsdp_bytes);
        info.has_xsdt = true;
        info.xsdt_phys = v2->xsdt_address;
        sum = checksum8(rsdp_bytes, v2->length);
    }

    info.checksum_ok = (sum == 0u);
    return true;
}

void log_madt(uint64_t madt_phys, kernel::boot::acpi::AcpiInfo& info) {
    const auto* madt = reinterpret_cast<const MadtHeader*>(static_cast<uintptr_t>(madt_phys));

    if (madt == nullptr || !text_equal4(madt->header.signature, "APIC")) {
        return;
    }

    info.madt_phys = madt_phys;
    info.local_apic_phys = madt->local_apic_address;

    const uint32_t total = madt->header.length;
    const uint8_t* cursor = reinterpret_cast<const uint8_t*>(madt) + sizeof(MadtHeader);
    const uint8_t* end = reinterpret_cast<const uint8_t*>(madt) + total;

    while (cursor + sizeof(MadtEntryHeader) <= end) {
        const auto* entry = reinterpret_cast<const MadtEntryHeader*>(cursor);

        if (entry->length < sizeof(MadtEntryHeader) || cursor + entry->length > end) {
            break;
        }

        if (entry->type == 1u) {
            // IOAPIC entry
            if (cursor + sizeof(MadtIoApicEntry) <= end) {
                const auto* io = reinterpret_cast<const MadtIoApicEntry*>(cursor);
                if (info.ioapic_count < 8u) {
                    info.ioapic_phys[info.ioapic_count] = io->ioapic_address;
                    info.ioapic_gsi_base[info.ioapic_count] = io->gsi_base;
                }
                ++info.ioapic_count;
            }
        }

        cursor += entry->length;
    }
}

void scan_acpi_table(uint64_t table_phys, bool is_xsdt, kernel::boot::acpi::AcpiInfo& info) {
    if (table_phys == 0) {
        return;
    }

    const auto* header = reinterpret_cast<const SdtHeader*>(static_cast<uintptr_t>(table_phys));

    if (header == nullptr || header->length < sizeof(SdtHeader)) {
        return;
    }

    const uint8_t* begin = reinterpret_cast<const uint8_t*>(header);
    const uint8_t* end = begin + header->length;

    if (checksum8(begin, header->length) != 0u) {
        return;
    }

    if (text_equal4(header->signature, "APIC")) {
        log_madt(table_phys, info);
        return;
    }

    const uint8_t* cursor = begin + sizeof(SdtHeader);

    while (cursor + (is_xsdt ? sizeof(uint64_t) : sizeof(uint32_t)) <= end) {
        uint64_t entry_phys = 0;

        if (is_xsdt) {
            entry_phys = *reinterpret_cast<const uint64_t*>(cursor);
            cursor += sizeof(uint64_t);

        } else {
            entry_phys = *reinterpret_cast<const uint32_t*>(cursor);
            cursor += sizeof(uint32_t);
        }

        const auto* entry_header = reinterpret_cast<const SdtHeader*>(static_cast<uintptr_t>(entry_phys));

        if (entry_header != nullptr && text_equal4(entry_header->signature, "APIC")) {
            log_madt(entry_phys, info);
            break;
        }
    }
}

}  // namespace

namespace kernel::boot::acpi {

bool discover(uint32_t multiboot_info_addr, AcpiInfo* out_info) {
    if (out_info == nullptr) {
        return false;
    }

    *out_info = {};

    const uint8_t* const info = reinterpret_cast<const uint8_t*>(multiboot_info_addr);

    if (info == nullptr) {
        return false;
    }

    const uint32_t total_size = *reinterpret_cast<const uint32_t*>(info);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(info) + 8u;
    const uintptr_t end = reinterpret_cast<uintptr_t>(info) + total_size;

    while (cursor + sizeof(TagHeader) <= end) {
        const auto* tag = reinterpret_cast<const TagHeader*>(cursor);

        if (tag->type == kTagTypeEnd) {
            break;
        }

        if ((tag->type == kTagTypeAcpiOld && tag->size >= sizeof(AcpiOldTag)) ||
            (tag->type == kTagTypeAcpiNew && tag->size >= sizeof(AcpiNewTag))) {
            const uint8_t* payload = reinterpret_cast<const uint8_t*>(cursor + 8u);
            const uint32_t payload_size = tag->size - 8u;

            if (parse_rsdp(payload, payload_size, *out_info)) {
                if (out_info->has_xsdt) {
                    scan_acpi_table(out_info->xsdt_phys, true, *out_info);
                } else {
                    scan_acpi_table(out_info->rsdt_phys, false, *out_info);
                }

                return true;
            }
        }

        cursor = align8(cursor + tag->size);
    }

    return false;
}

void log(uint32_t multiboot_info_addr) {
    AcpiInfo info = {};

    if (!discover(multiboot_info_addr, &info)) {
        kernel::serial::write("[wirth] acpi: not found\n");
        return;
    }

    kernel::serial::write("[wirth] acpi rsdp=0x");
    kernel::serial::write_hex64(info.rsdp_phys);
    kernel::serial::write(" checksum=");
    kernel::serial::write(info.checksum_ok ? "ok" : "bad");
    kernel::serial::write(" rsdt=0x");
    kernel::serial::write_hex64(info.rsdt_phys);

    if (info.has_xsdt) {
        kernel::serial::write(" xsdt=0x");
        kernel::serial::write_hex64(info.xsdt_phys);
    }

    if (info.madt_phys != 0) {
        kernel::serial::write(" madt=0x");
        kernel::serial::write_hex64(info.madt_phys);
        kernel::serial::write(" lapic=0x");
        kernel::serial::write_hex(info.local_apic_phys);
        kernel::serial::write(" ioapic=");
        kernel::serial::write_hex(info.ioapic_count);
    }

    kernel::serial::write("\n");
}

}  // namespace kernel::boot::acpi