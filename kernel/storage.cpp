#include "storage.hpp"
#include "drivers/ide.hpp"
#include "nvme.hpp"
#include "serial.hpp"
#include "block.hpp"
#include "fs/vfs.hpp"
#include "fs/ramfs.hpp"
#include "mm/heap.hpp"

static void kmemcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dst);
    const uint8_t* s = reinterpret_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
}

static void kmemset(void* dst, int c, uint32_t n) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dst);
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = static_cast<uint8_t>(c);
    }
}

static uint32_t text_len(const char* s) {
    if (s == nullptr) {
        return 0;
    }

    uint32_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }

    return n;
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
    // Prefer a registered block device if available
    kernel::BlockDevice* dev = kernel::block_get_primary();

    if (dev == nullptr) {
        (void)kernel::nvme::init();
        dev = kernel::block_get_primary();
    }

    if (dev == nullptr) {
        // fallback to legacy IDE
        ide_init();

        if (!ide_present()) {

            sb.magic = 0;
            sb.next_free_lba = DATA_START_LBA;
            g_storage_ready = false;

            return;
        }

        g_storage_ready = true;
        // register IDE as primary block device
        static kernel::BlockDevice ide_block = {};

        auto ide_read_wrapper = [](uint64_t lba, uint8_t* buf, uint32_t count) -> bool {
            for (uint32_t i = 0; i < count; ++i) {
                if (!ide_read_sectors(static_cast<uint32_t>(lba + i), buf + i * 512, 1)) return false;
            }
            return true;
        };

        auto ide_write_wrapper = [](uint64_t lba, const uint8_t* buf, uint32_t count) -> bool {
            for (uint32_t i = 0; i < count; ++i) {
                if (!ide_write_sectors(static_cast<uint32_t>(lba + i), buf + i * 512, 1)) return false;
            }
            return true;
        };

        ide_block.name = "ide0";
        ide_block.block_size = 512;
        ide_block.block_count = 0; // unknown
        ide_block.read_sectors = reinterpret_cast<bool (*)(uint64_t, uint8_t*, uint32_t)>(+ide_read_wrapper);
        ide_block.write_sectors = reinterpret_cast<bool (*)(uint64_t, const uint8_t*, uint32_t)>(+ide_write_wrapper);
        ide_block.ctx = nullptr;

        (void)kernel::block_register_device(&ide_block);
        uint8_t buf[512];

        if (!ide_read_sectors(SUPERBLOCK_LBA, buf, 1)) {
        // cannot read superblock; will initialize a new one
            sb.magic = 0;
            sb.next_free_lba = DATA_START_LBA;

            return;
    }
        kmemcpy(&sb, buf, sizeof(Superblock));

        if (sb.magic != MAGIC || sb.next_free_lba < DATA_START_LBA) {
            // formatting new superblock

            sb.magic = MAGIC;
            sb.next_free_lba = DATA_START_LBA;

            kmemset(sb.reserved, 0, sizeof(sb.reserved));
            kmemcpy(buf, &sb, sizeof(Superblock));
            ide_write_sectors(SUPERBLOCK_LBA, buf, 1);

        }

    } else {
        // initialize from primary block device
        g_storage_ready = true;
        uint8_t buf[512];

        if (!dev->read_sectors(SUPERBLOCK_LBA, buf, 1)) {
            sb.magic = 0;
            sb.next_free_lba = DATA_START_LBA;
            return;
        }

        kmemcpy(&sb, buf, sizeof(Superblock));

        if (sb.magic != MAGIC || sb.next_free_lba < DATA_START_LBA) {
            sb.magic = MAGIC;
            sb.next_free_lba = DATA_START_LBA;
            kmemset(sb.reserved, 0, sizeof(sb.reserved));
            kmemcpy(buf, &sb, sizeof(Superblock));
            dev->write_sectors(SUPERBLOCK_LBA, buf, 1);
        }
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

    kernel::BlockDevice* dev = kernel::block_get_primary();
    if (dev) return dev->write_sectors(SUPERBLOCK_LBA, buf, 1);
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
        kernel::BlockDevice* dev = kernel::block_get_primary();
        if (dev) {
            if (!dev->write_sectors(start + i, tmp, 1)) return false;
        } else {
            if (!ide_write_sectors(start + i, tmp, 1)) return false;
        }

        size -= copy;
    }

    sb.next_free_lba += sectors;
    uint8_t buf[512];

    kmemcpy(buf, &sb, sizeof(Superblock));

    kernel::BlockDevice* dev = kernel::block_get_primary();
    if (dev) {
        if (!dev->write_sectors(SUPERBLOCK_LBA, buf, 1)) return false;
    } else {
        if (!ide_write_sectors(SUPERBLOCK_LBA, buf, 1)) return false;
    }

    if (out) { out->start_lba = start; out->sectors = sectors; }

    return true;
}

bool storage_read(const StorageHandle& h, uint8_t* out_buf) {

    if (h.sectors == 0) return false;

    kernel::BlockDevice* dev = kernel::block_get_primary();
    if (dev) {
        return dev->read_sectors(h.start_lba, out_buf, h.sectors);
    }

    for (uint32_t i = 0; i < h.sectors; ++i) {
        if (!ide_read_sectors(h.start_lba + i, out_buf + i * 512, 1)) return false;
    }

    return true;
}

bool storage_read_raw(uint32_t start_lba, uint32_t sectors, uint8_t* out_buf) {
    kernel::BlockDevice* dev = kernel::block_get_primary();
    if (dev) return dev->read_sectors(start_lba, out_buf, sectors);

    for (uint32_t i = 0; i < sectors; ++i) {
        if (!ide_read_sectors(start_lba + i, out_buf + i * 512, 1)) return false;
    }

    return true;
}

bool storage_add_index_entry(const char* name, uint32_t start_lba, uint32_t sectors) {
    uint8_t index_buf[512];

    // read existing index (if possible)
    if (!storage_read_raw(INDEX_LBA, 1, index_buf)) {
        for (uint32_t i = 0; i < 512; ++i) {
            index_buf[i] = 0;
        }
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

    for (uint32_t i = 0; i < name_len; ++i) {
        index_buf[off + 12 + i] = name[i];
    }

    off += need;
    entry_count += 1;

    *(uint32_t*)(index_buf) = entry_count;

    // write back
    kernel::BlockDevice* dev = kernel::block_get_primary();
    if (dev) {
        if (!dev->write_sectors(INDEX_LBA, index_buf, 1)) return false;
    } else {
        if (!ide_write_sectors(INDEX_LBA, index_buf, 1)) return false;
    }
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
        uint8_t* data = nullptr;
    #if defined(__x86_64__) || defined(__LP64__)
        data = new uint8_t[sectors * 512];
    #else
        data = reinterpret_cast<uint8_t*>(kernel::mm::heap::alloc(sectors * 512, 4096));
    #endif
 
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
 
            // ensure parent directories exist before creating file
            // build parent prefix and md progressively
            if (path[0] != '\0') {
                char prefix[256] = {};
                uint32_t p = 0;
                // always start with '/'
                prefix[p++] = '/';
                for (uint32_t pi = 1; pi < text_len(path); ++pi) {
                    if (path[pi] == '/') {
                        prefix[p] = '\0';
                        (void)kernel::fs::g_fs->md(prefix);
                        if (p + 1 < sizeof(prefix)) { prefix[p++] = '/'; }
                        continue;
                    }
                    if (p + 1 < sizeof(prefix)) prefix[p++] = path[pi];
                }
            }

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
 
        while (prefix[mw] != '\0') {
            marker[mw] = prefix[mw];
            ++mw;
        }

        uint32_t m_name_len = 0;
        while (name[m_name_len] != '\0') {
            ++m_name_len;
        }

        uint32_t m_copy = (m_name_len < 96) ? m_name_len : 96;

        for (uint32_t k = 0; k < m_copy; ++k) {
            marker[mw + k] = name[k];
        }

        mw += m_copy;

        if (mw + 9 < sizeof(marker)) {
            marker[mw++] = '.';
            marker[mw++] = 'i';
            marker[mw++] = 'n';
            marker[mw++] = 's';
            marker[mw++] = 't';
            marker[mw++] = 'a';
            marker[mw++] = 'l';
            marker[mw++] = 'l';
            marker[mw++] = 'e';
            marker[mw++] = 'd';
            marker[mw] = '\0';

            int mfd = kernel::fs::g_fs->open(marker, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate);

            if (mfd >= 0) {
                kernel::fs::g_fs->close(mfd);
            }
        }
 
        (void)data; // allocation not freed; heap has no free
    }
}

bool storage_snapshot_save(const char* name) {
    if (!g_storage_ready || kernel::fs::g_fs == nullptr || name == nullptr) return false;

    // Two-pass: first compute total size needed, then allocate and fill payload.
    uint32_t total = 0;

    // recursive lambda-like functions (implemented as local static functions)
    // compute size
    struct Walker {
        static bool compute(const char* path, uint32_t& total) {
            kernel::fs::DirEntry entries[64] = {};
            int cnt = kernel::fs::g_fs->readdir(path, entries, 64);
            if (cnt < 0) return true;

            for (int i = 0; i < cnt; ++i) {
                char child[256] = {};
                uint32_t pos = 0;
                if (path[0] == '/' && path[1] == '\0') {
                    child[pos++] = '/';
                    uint32_t j = 0;
                    while (entries[i].name[j] != '\0' && pos + 1 < sizeof(child)) child[pos++] = entries[i].name[j++];
                    child[pos] = '\0';
                } else {
                    uint32_t j = 0;
                    while (path[j] != '\0' && pos + 1 < sizeof(child)) child[pos++] = path[j++];
                    if (pos + 1 < sizeof(child)) child[pos++] = '/';
                    uint32_t j2 = 0;
                    while (entries[i].name[j2] != '\0' && pos + 1 < sizeof(child)) child[pos++] = entries[i].name[j2++];
                    child[pos] = '\0';
                }

                if (entries[i].is_directory) {
                    // recurse
                    if (!compute(child, total)) return false;
                } else {
                    // file: open and measure
                    int fd = kernel::fs::g_fs->open(child, kernel::fs::kOpenRead);
                    if (fd < 0) continue;
                    uint8_t buf[512];
                    uint32_t file_len = 0;
                    while (true) {
                        int n = kernel::fs::g_fs->read(fd, buf, 512);
                        if (n < 0) { kernel::fs::g_fs->close(fd); return false; }
                        if (n == 0) break;
                        file_len += static_cast<uint32_t>(n);
                    }
                    kernel::fs::g_fs->close(fd);

                    uint32_t path_len = text_len(child);
                    total += 8 + path_len + file_len;
                }
            }

            return true;
        }

        static bool fill(const char* path, uint8_t* payload, uint32_t payload_size, uint32_t& off) {
            kernel::fs::DirEntry entries[64] = {};
            int cnt = kernel::fs::g_fs->readdir(path, entries, 64);
            if (cnt < 0) return true;

            for (int i = 0; i < cnt; ++i) {
                char child[256] = {};
                uint32_t pos = 0;
                if (path[0] == '/' && path[1] == '\0') {
                    child[pos++] = '/';
                    uint32_t j = 0;
                    while (entries[i].name[j] != '\0' && pos + 1 < sizeof(child)) child[pos++] = entries[i].name[j++];
                    child[pos] = '\0';
                } else {
                    uint32_t j = 0;
                    while (path[j] != '\0' && pos + 1 < sizeof(child)) child[pos++] = path[j++];
                    if (pos + 1 < sizeof(child)) child[pos++] = '/';
                    uint32_t j2 = 0;
                    while (entries[i].name[j2] != '\0' && pos + 1 < sizeof(child)) child[pos++] = entries[i].name[j2++];
                    child[pos] = '\0';
                }

                if (entries[i].is_directory) {
                    if (!fill(child, payload, payload_size, off)) return false;
                } else {
                    int fd = kernel::fs::g_fs->open(child, kernel::fs::kOpenRead);
                    if (fd < 0) continue;

                    // compute file length by reading into temporary buffer and copying into payload
                    uint8_t tmp[512];
                    uint32_t file_len = 0;

                    // reserve header
                    uint32_t path_len = text_len(child);

                    if (off + 8 + path_len > payload_size) { kernel::fs::g_fs->close(fd); return false; }

                    uint32_t header_pos = off;
                    *(uint32_t*)(payload + header_pos) = path_len;
                    *(uint32_t*)(payload + header_pos + 4) = 0; // placeholder for data_len
                    off = header_pos + 8;

                    // copy path
                    for (uint32_t k = 0; k < path_len; ++k) payload[off + k] = static_cast<uint8_t>(child[k]);
                    off += path_len;

                    while (true) {
                        int n = kernel::fs::g_fs->read(fd, tmp, sizeof(tmp));
                        if (n < 0) { kernel::fs::g_fs->close(fd); return false; }
                        if (n == 0) break;
                        if (off + static_cast<uint32_t>(n) > payload_size) { kernel::fs::g_fs->close(fd); return false; }
                        for (int b = 0; b < n; ++b) payload[off + b] = tmp[b];
                        off += static_cast<uint32_t>(n);
                        file_len += static_cast<uint32_t>(n);
                    }

                    // write actual data_len
                    *(uint32_t*)(payload + header_pos + 4) = file_len;
                    kernel::fs::g_fs->close(fd);
                }
            }

            return true;
        }
    };

    // compute total size starting at root
    if (!Walker::compute("/", total)) return false;

    if (total == 0) return false;

    uint8_t* payload = nullptr;
#if defined(__x86_64__) || defined(__LP64__)
    payload = new uint8_t[total];
#else
    payload = reinterpret_cast<uint8_t*>(kernel::mm::heap::alloc(total, 4096));
#endif
    if (payload == nullptr) return false;

    uint32_t off = 0;
    if (!Walker::fill("/", payload, total, off)) {
        return false;
    }

    if (off != total) {
        // mismatch; but still try to write what we have
    }

    StorageHandle h = {};
    if (!storage_write(payload, off, &h)) {
        return false;
    }

    if (!storage_add_index_entry(name, h.start_lba, h.sectors)) {
        return false;
    }

    return true;
}

} // namespace kernel
