#include "storage.hpp"
#include "drivers/ide.hpp"
#include "serial.hpp"
#include "fs/vfs.hpp"
#include "fs/ramfs.hpp"
#include "mm/heap.hpp"

static void kmemcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dst);
    const uint8_t* s = reinterpret_cast<const uint8_t*>(src);

    for (uint32_t i = 0; i < n; ++i) d[i] = s[i];
}

static void kmemset(void* dst, int c, uint32_t n) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dst);

    for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(c);
}

using namespace kernel::drivers;

namespace kernel {

static const uint32_t SUPERBLOCK_LBA = 1;
static const uint32_t DATA_START_LBA = 10;
static const uint32_t INDEX_LBA = 2;
static const uint32_t MAGIC = 0x484F4B55; // 'HOKU'

struct Superblock {
    uint32_t magic;
    uint32_t next_free_lba;
    uint8_t reserved[504];
};

static Superblock sb;
static bool g_storage_ready = false;

void storage_init() {
    serial::write("storage: init\n");
    ide_init();
 
    if (!ide_present()) {
        serial::write("storage: no IDE device detected\n");
 
        sb.magic = 0;
        sb.next_free_lba = DATA_START_LBA;
        g_storage_ready = false;
 
        return;
    }

    g_storage_ready = true;
    uint8_t buf[512];

    if (!ide_read_sectors(SUPERBLOCK_LBA, buf, 1)) {
        serial::write("storage: cannot read superblock\n");

        sb.magic = 0;
        sb.next_free_lba = DATA_START_LBA;

        return;
    }

    kmemcpy(&sb, buf, sizeof(Superblock));

    if (sb.magic != MAGIC || sb.next_free_lba < DATA_START_LBA) {
        serial::write("storage: formatting new superblock\n");

        sb.magic = MAGIC;
        sb.next_free_lba = DATA_START_LBA;

        kmemset(sb.reserved, 0, sizeof(sb.reserved));
        kmemcpy(buf, &sb, sizeof(Superblock));
        ide_write_sectors(SUPERBLOCK_LBA, buf, 1);

    } else {
        serial::write("storage: superblock loaded\n");
    }
}

bool storage_ready() {
    return g_storage_ready;
}

uint32_t storage_data_start_lba() {
    return DATA_START_LBA;
}

uint32_t storage_next_free_lba() {
    return sb.next_free_lba;
}

uint32_t storage_used_sectors() {

    if (sb.next_free_lba <= DATA_START_LBA) {
        return 0;
    }

    return sb.next_free_lba - DATA_START_LBA;
}

bool storage_flush() {
    uint8_t buf[512];
    kmemcpy(buf, &sb, sizeof(Superblock));

    return ide_write_sectors(SUPERBLOCK_LBA, buf, 1);
}

bool storage_write(const uint8_t* data, uint32_t size, StorageHandle* out) {
    if (size == 0 || data == nullptr) return false;

    uint32_t sectors = (size + 511) / 512;
    uint32_t start = sb.next_free_lba;

    for (uint32_t i = 0; i < sectors; ++i) {
        uint8_t tmp[512];
        uint32_t copy = (size > 512) ? 512 : size;

        kmemcpy(tmp, data + i * 512, copy);

        if (copy < 512) kmemset(tmp + copy, 0, 512 - copy);
        if (!ide_write_sectors(start + i, tmp, 1)) return false;

        size -= copy;
    }

    sb.next_free_lba += sectors;
    uint8_t buf[512];

    kmemcpy(buf, &sb, sizeof(Superblock));

    if (!ide_write_sectors(SUPERBLOCK_LBA, buf, 1)) return false;

    if (out) { out->start_lba = start; out->sectors = sectors; }

    return true;
}

bool storage_read(const StorageHandle& h, uint8_t* out_buf) {

    if (h.sectors == 0) return false;

    for (uint32_t i = 0; i < h.sectors; ++i) {
        if (!ide_read_sectors(h.start_lba + i, out_buf + i * 512, 1)) return false;
    }

    return true;
}

bool storage_read_raw(uint32_t start_lba, uint32_t sectors, uint8_t* out_buf) {
    for (uint32_t i = 0; i < sectors; ++i) {
        if (!ide_read_sectors(start_lba + i, out_buf + i * 512, 1)) return false;
    }

    return true;
}

bool storage_add_index_entry(const char* name, uint32_t start_lba, uint32_t sectors) {
    uint8_t index_buf[512];

    // read existing index (if possible)
    if (!storage_read_raw(INDEX_LBA, 1, index_buf)) {
        for (uint32_t i = 0; i < 512; ++i) index_buf[i] = 0;
    }

    uint32_t entry_count = *(uint32_t*)(index_buf);
    uint32_t off = 4;

    for (uint32_t e = 0; e < entry_count; ++e) {

        if (off + 12 > 512) return false;

        uint32_t name_len = *(uint32_t*)(index_buf + off);
        off += 12 + name_len;

        if (off > 512) return false;
    }

    uint32_t name_len = 0;

    while (name[name_len] != '\0') ++name_len;

    uint32_t need = 12 + name_len;

    if (off + need > 512) return false;

    *(uint32_t*)(index_buf + off) = name_len;
    *(uint32_t*)(index_buf + off + 4) = start_lba;
    *(uint32_t*)(index_buf + off + 8) = sectors;

    for (uint32_t i = 0; i < name_len; ++i) index_buf[off + 12 + i] = name[i];

    off += need;
    entry_count += 1;

    *(uint32_t*)(index_buf) = entry_count;

    // write back
    if (!ide_write_sectors(INDEX_LBA, index_buf, 1)) return false;
    return true;
}

void storage_restore_packages() {
    // Read index from fixed LBA and restore each entry
    uint8_t index_buf[512];
 
    if (!storage_read_raw(INDEX_LBA, 1, index_buf)) return;
 
    // layout: uint32_t entry_count;
    uint32_t entry_count = *(uint32_t*)(index_buf);
    uint32_t off = 4;
 
    for (uint32_t e = 0; e < entry_count; ++e) {
 
        if (off + 12 > 512) break;
 
        uint32_t name_len = *(uint32_t*)(index_buf + off);
        uint32_t start = *(uint32_t*)(index_buf + off + 4);
        uint32_t sectors = *(uint32_t*)(index_buf + off + 8);
 
        off += 12;
 
        if (name_len == 0 || off + name_len > 512) break;
 
        char name[128] = {};
 
        uint32_t copy = (name_len < sizeof(name)-1) ? name_len : sizeof(name)-1;
 
        for (uint32_t k = 0; k < copy; ++k) name[k] = index_buf[off + k];
 
        off += name_len;

        // Read payload and restore
        uint8_t* data = reinterpret_cast<uint8_t*>(kernel::mm::heap::alloc(sectors * 512, 4096));
 
        if (data == nullptr) continue;
 
        if (!storage_read_raw(start, sectors, data)) {
            continue;
        }
 
        uint32_t po = 0;
 
        while (po + 8 <= sectors * 512) {
            uint32_t path_len = *(uint32_t*)(data + po);
            uint32_t data_len = *(uint32_t*)(data + po + 4);
 
            po += 8;
 
            if (path_len == 0 || po + path_len + data_len > sectors * 512) break;
 
            char path[256] = {};
 
            uint32_t copy_path = (path_len < sizeof(path)-1) ? path_len : sizeof(path)-1;
 
            for (uint32_t k = 0; k < copy_path; ++k) path[k] = data[po + k];
            po += path_len;
 
            int ofd = kernel::fs::g_fs->open(path, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate);
 
            if (ofd >= 0) {
                kernel::fs::g_fs->write(ofd, data + po, data_len);
                kernel::fs::g_fs->close(ofd);
            }
 
            po += data_len;
        }
 
        // create installed marker in pkgdb
        char marker[128] = {};
        uint32_t mw = 0;
        const char* prefix = "/var/lib/wirth/pkgdb/";
 
        while (prefix[mw] != '\0') { marker[mw] = prefix[mw]; ++mw; }
 
        uint32_t m_name_len = 0; while (name[m_name_len] != '\0') ++m_name_len;
        uint32_t m_copy = (m_name_len < 96) ? m_name_len : 96;
 
        for (uint32_t k = 0; k < m_copy; ++k) marker[mw + k] = name[k];
        mw += m_copy;
 
        if (mw + 9 < sizeof(marker)) {
            marker[mw++] = '.'; marker[mw++] = 'i'; marker[mw++] = 'n'; marker[mw++] = 's'; marker[mw++] = 't'; marker[mw++] = 'a'; marker[mw++] = 'l'; marker[mw++] = 'l'; marker[mw++] = 'e'; marker[mw++] = 'd';
            marker[mw] = '\0';
 
            int mfd = kernel::fs::g_fs->open(marker, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate);
 
            if (mfd >= 0) kernel::fs::g_fs->close(mfd);
        }
 
        (void)data; // allocation not freed; heap has no free
    }
}

} // namespace kernel
