#include "block_partition.hpp"

#include "block.hpp"
#include "serial.hpp"

namespace {

constexpr uint32_t kMaxPartitions = 8;
constexpr uint32_t kMbrSignatureOffset = 510;
constexpr uint8_t kPartitionTypeEmpty = 0x00;

struct [[gnu::packed]] MbrPartitionEntry {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
};

struct PartitionRecord {
    bool used;
    kernel::BlockDevice device;
    kernel::BlockDevice* base;
    uint64_t start_lba;
    uint64_t sector_count;
};

PartitionRecord g_partitions[kMaxPartitions] = {};
char g_partition_names[kMaxPartitions][16] = {};

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void copy_text(char* dst, const char* src, uint32_t max_size) {
    if (dst == nullptr || src == nullptr || max_size == 0) {
        return;
    }

    uint32_t i = 0;

    while (i + 1 < max_size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

uint32_t text_len(const char* text) {
    uint32_t n = 0;

    while (text != nullptr && text[n] != '\0') {
        ++n;
    }

    return n;
}

bool read_partition_impl(uint32_t slot, uint64_t lba, uint8_t* buf, uint32_t count) {
    if (slot >= kMaxPartitions || !g_partitions[slot].used || g_partitions[slot].base == nullptr) {
        return false;
    }

    const PartitionRecord& rec = g_partitions[slot];

    if (lba + count > rec.sector_count) {
        return false;
    }

    return rec.base->read_sectors(rec.start_lba + lba, buf, count);
}

bool write_partition_impl(uint32_t slot, uint64_t lba, const uint8_t* buf, uint32_t count) {
    if (slot >= kMaxPartitions || !g_partitions[slot].used || g_partitions[slot].base == nullptr) {
        return false;
    }

    const PartitionRecord& rec = g_partitions[slot];

    if (lba + count > rec.sector_count) {
        return false;
    }

    if (rec.base->write_sectors == nullptr) {
        return false;
    }

    return rec.base->write_sectors(rec.start_lba + lba, buf, count);
}

#define DECL_PARTITION_SLOT(N) \
    static bool read_partition_##N(uint64_t lba, uint8_t* buf, uint32_t count) { \
        return read_partition_impl(N, lba, buf, count); \
    } \
    static bool write_partition_##N(uint64_t lba, const uint8_t* buf, uint32_t count) { \
        return write_partition_impl(N, lba, buf, count); \
    }

DECL_PARTITION_SLOT(0)
DECL_PARTITION_SLOT(1)
DECL_PARTITION_SLOT(2)
DECL_PARTITION_SLOT(3)
DECL_PARTITION_SLOT(4)
DECL_PARTITION_SLOT(5)
DECL_PARTITION_SLOT(6)
DECL_PARTITION_SLOT(7)

using ReadFn = bool (*)(uint64_t, uint8_t*, uint32_t);
using WriteFn = bool (*)(uint64_t, const uint8_t*, uint32_t);

const ReadFn kReadFns[kMaxPartitions] = {
    read_partition_0, read_partition_1, read_partition_2, read_partition_3,
    read_partition_4, read_partition_5, read_partition_6, read_partition_7,
};

const WriteFn kWriteFns[kMaxPartitions] = {
    write_partition_0, write_partition_1, write_partition_2, write_partition_3,
    write_partition_4, write_partition_5, write_partition_6, write_partition_7,
};

uint32_t find_free_slot() {
    for (uint32_t i = 0; i < kMaxPartitions; ++i) {
        if (!g_partitions[i].used) {
            return i;
        }
    }

    return kMaxPartitions;
}

void build_partition_name(const char* base_name, uint32_t index, char* out, uint32_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';

    if (base_name != nullptr && base_name[0] != '\0') {
        copy_text(out, base_name, out_size);

    } else {
        copy_text(out, "disk", out_size);
    }

    const uint32_t prefix_len = text_len(out);

    if (prefix_len + 3 >= out_size) {
        return;
    }

    out[prefix_len + 0] = 'p';
    out[prefix_len + 1] = static_cast<char>('0' + ((index + 1u) / 10u) % 10u);
    out[prefix_len + 2] = static_cast<char>('0' + ((index + 1u) % 10u));
    out[prefix_len + 3] = '\0';

    if (index + 1u < 10u) {
        out[prefix_len + 1] = static_cast<char>('0' + (index + 1u));
        out[prefix_len + 2] = '\0';
    }
}

}  // namespace

namespace kernel::block_partition {

bool scan_mbr_partitions(BlockDevice* base) {
    if (base == nullptr || base->read_sectors == nullptr) {
        return false;
    }

    uint8_t mbr[512] = {};

    if (!base->read_sectors(0, mbr, 1)) {
        kernel::serial::write("[block-partition]: failed to read MBR\n");
        return false;
    }

    if (mbr[kMbrSignatureOffset] != 0x55u || mbr[kMbrSignatureOffset + 1] != 0xAAu) {
        return false;
    }

    const uint32_t base_name_len = text_len(base->name);

    uint32_t found = 0;

    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t* entry = mbr + 446u + i * 16u;
        const uint8_t type = entry[4];
        const uint32_t start_lba = read_le32(entry + 8);
        const uint32_t sectors = read_le32(entry + 12);

        if (type == kPartitionTypeEmpty || sectors == 0u) {
            continue;
        }

        const uint32_t slot = find_free_slot();

        if (slot >= kMaxPartitions) {
            kernel::serial::write("[block-partition]: partition table full\n");
            break;
        }

        PartitionRecord& rec = g_partitions[slot];
        rec.used = true;
        rec.base = base;
        rec.start_lba = start_lba;
        rec.sector_count = sectors;

        build_partition_name(base->name, i, g_partition_names[slot], sizeof(g_partition_names[slot]));

        rec.device.name = g_partition_names[slot];
        rec.device.block_size = base->block_size;
        rec.device.block_count = sectors;
        rec.device.read_sectors = kReadFns[slot];
        rec.device.write_sectors = (base->write_sectors != nullptr) ? kWriteFns[slot] : nullptr;
        rec.device.ctx = nullptr;

        if (kernel::block_register_device(&rec.device)) {
            ++found;
            kernel::serial::write("[block-partition]: ");
            kernel::serial::write(rec.device.name);
            kernel::serial::write(" start_lba=");
            kernel::serial::write_hex64(start_lba);
            kernel::serial::write(" sectors=");
            kernel::serial::write_hex64(sectors);
            kernel::serial::write(" type=0x");
            kernel::serial::write_hex(type);
            kernel::serial::write("\n");

        } else {
            rec.used = false;
        }
    }

    return found != 0u;
}

}  // namespace kernel::block_partition