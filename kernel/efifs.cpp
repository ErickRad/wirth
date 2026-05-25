#include "efifs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "usb_mass_storage.hpp"
#include "mm/heap.hpp"
#include "serial.hpp"
#include "fs/vfs.hpp"
#include "drivers/ide.hpp"

namespace {

static uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Wrappers to adapt various block backends to a common signature used by EFIFS.
static bool efifs_read_usb(uint64_t lba, uint8_t* buf, uint32_t cnt) {
    return kernel::usbms::read_sectors(lba, buf, cnt);
}

static bool efifs_read_ide(uint64_t lba, uint8_t* buf, uint32_t cnt) {
    return kernel::drivers::ide_read_sectors(static_cast<uint32_t>(lba), buf, cnt);
}

// Normalize an 11-byte FAT name into a null-terminated string.
// Compare a directory name with a target like "BOOT    " or "ROOTFSSEED" (11 bytes)
static bool fat11_equal(const uint8_t* entry, const char* expected11) {
    for (int i = 0; i < 11; ++i) {
        if (entry[i] != static_cast<uint8_t>(expected11[i])) return false;
    }
    return true;
}

} // namespace

namespace kernel {
namespace efifs {

bool try_load_rootfs_seed_from_esp() {
    // Select block device read backend: prefer USB mass-storage, fall back to IDE.
    using ReadFn = bool(*)(uint64_t, uint8_t*, uint32_t);

    ReadFn read_sectors = nullptr;
    uint8_t sector[512];
    bool got_sector = false;

    // Try USB first if available; if it fails, fall back to IDE.
    if (kernel::usbms::present()) {
        if (efifs_read_usb(0, sector, 1)) {
            read_sectors = &efifs_read_usb;
            got_sector = true;
        } else {
            kernel::serial::write("[efifs]: usb read failed, trying IDE\n");
        }
    }

    if (!got_sector && kernel::drivers::ide_present()) {
        if (efifs_read_ide(0, sector, 1)) {
            read_sectors = &efifs_read_ide;
            got_sector = true;
        } else {
            kernel::serial::write("[efifs]: ide read failed\n");
        }
    }

    if (!got_sector) {
        kernel::serial::write("[efifs]: failed to read MBR/boot sector\n");
        return false;
    }

    const uint16_t bytes_per_sector = read_u16(sector + 11);
    const uint8_t sectors_per_cluster = sector[13];
    const uint16_t reserved_sectors = read_u16(sector + 14);
    const uint8_t num_fats = sector[16];
    const uint16_t root_entry_count = read_u16(sector + 17);
    const uint16_t sectors_per_fat = read_u16(sector + 22);

    // Debug: report BPB bytes-per-sector for diagnosis
    kernel::serial::write("[efifs]: BPB bytes/sector=");
    kernel::serial::write_hex((uint32_t)bytes_per_sector);
    kernel::serial::write(" sectors/cluster=");
    kernel::serial::write_hex((uint32_t)sectors_per_cluster);
    kernel::serial::write(" reserved=");
    kernel::serial::write_hex((uint32_t)reserved_sectors);
    kernel::serial::write(" fats=");
    kernel::serial::write_hex((uint32_t)num_fats);
    kernel::serial::write(" root_entries=");
    kernel::serial::write_hex((uint32_t)root_entry_count);
    kernel::serial::write(" spf=");
    kernel::serial::write_hex((uint32_t)sectors_per_fat);
    kernel::serial::write("\n");

    kernel::serial::write("[efifs]: sector[0..15]=");
    for (int i = 0; i < 16; ++i) {
        kernel::serial::write_hex((uint32_t)sector[i]);
        kernel::serial::write(" ");
    }
    kernel::serial::write("\n");

    if (bytes_per_sector != 512) {
        kernel::serial::write("[efifs]: unsupported bytes/sector (only 512)\n");
        return false;
    }

    const uint32_t root_dir_sectors = ((static_cast<uint32_t>(root_entry_count) * 32u) + bytes_per_sector - 1u) / bytes_per_sector;
    const uint32_t first_root_dir_sector = reserved_sectors + static_cast<uint32_t>(num_fats) * sectors_per_fat;
    const uint32_t first_data_sector = first_root_dir_sector + root_dir_sectors;

    // Read root directory into a small static scratch buffer to avoid heap usage
    const uint32_t root_bytes = root_dir_sectors * bytes_per_sector;
    static uint8_t root_buf[16 * 1024];

    if (root_bytes > sizeof(root_buf)) {
        kernel::serial::write("[efifs]: root dir too large\n");
        return false;
    }

    for (uint32_t i = 0; i < root_dir_sectors; ++i) {
        if (!read_sectors(first_root_dir_sector + i, root_buf + i * bytes_per_sector, 1)) {
            kernel::serial::write("[efifs]: failed reading root dir\n");
            return false;
        }
    }

    // Find BOOT directory entry in root
    uint16_t boot_dir_cluster = 0;

    for (uint32_t i = 0; i < root_entry_count; ++i) {
        const uint8_t* e = root_buf + i * 32u;

        if (e[0] == 0x00) break; // no more
        if (e[0] == 0xE5) continue; // deleted

        const uint8_t attr = e[11];

        if ((attr & 0x10u) == 0) continue; // not a directory

        // name for comparison: "BOOT       " (11 bytes)
        if (fat11_equal(e, "BOOT       ")) {
            boot_dir_cluster = read_u16(e + 26);
            break;
        }
    }

    if (boot_dir_cluster == 0) {
        kernel::serial::write("[efifs]: BOOT dir not found in root\n");
        return false;
    }

    // Read FAT into memory
    const uint32_t fat_bytes = static_cast<uint32_t>(sectors_per_fat) * bytes_per_sector;
    static uint8_t fat_buf[16 * 1024];

    if (fat_bytes > sizeof(fat_buf)) {
        kernel::serial::write("[efifs]: FAT too large\n");
        return false;
    }

    for (uint32_t i = 0; i < sectors_per_fat; ++i) {
        if (!read_sectors(reserved_sectors + i, fat_buf + i * bytes_per_sector, 1)) {
            kernel::serial::write("[efifs]: failed reading FAT\n");
            return false;
        }
    }

    // Helper to read next cluster from FAT16
    auto next_cluster = [&](uint16_t c) -> uint16_t {
        const uint32_t off = static_cast<uint32_t>(c) * 2u;
        if (off + 1u >= fat_bytes) return 0xFFFFu;
        return static_cast<uint16_t>(fat_buf[off]) | (static_cast<uint16_t>(fat_buf[off + 1]) << 8);
    };

    // Read the BOOT directory cluster chain and search for ROOTFS.SEED
    // read a few clusters at a time (most dirs are small)
    uint16_t c = boot_dir_cluster;
    bool found = false;
    uint32_t file_start_cluster = 0;
    uint32_t file_size = 0;

    while (c >= 2 && c < 0xFFF8u) {
        const uint32_t sector0 = first_data_sector + (static_cast<uint32_t>(c) - 2u) * sectors_per_cluster;
        for (uint32_t s = 0; s < sectors_per_cluster; ++s) {
            uint8_t dir_sector[512];
            if (!read_sectors(sector0 + s, dir_sector, 1)) {
                kernel::serial::write("[efifs]: failed reading BOOT dir cluster\n");
                return false;
            }

            const uint32_t entries = 512 / 32;
            for (uint32_t eidx = 0; eidx < entries; ++eidx) {
                const uint8_t* e = dir_sector + eidx * 32u;

                if (e[0] == 0x00) break;
                if (e[0] == 0xE5) continue;

                const uint8_t attr = e[11];
                if ((attr & 0x08u) != 0) continue; // volume label

                // target file name is ROOTFS (6 chars) and ext SEED -> combined entry bytes == "ROOTFSSEED"
                // In the image generator we wrote name "ROOTFS" ext "SEED"
                if (fat11_equal(e, "ROOTFSSEED")) {
                    file_start_cluster = read_u16(e + 26);
                    file_size = read_u32(e + 28);
                    found = true;
                    break;
                }
            }

            if (found) break;
        }

        if (found) break;
        c = next_cluster(c);
        if (c == 0xFFFFu) break;
    }

    if (!found || file_start_cluster == 0) {
        kernel::serial::write("[efifs]: ROOTFS.SEED not found in BOOT dir\n");
        return false;
    }

    // Read the file cluster chain into a buffer
    uint32_t remaining = file_size;
    static uint8_t file_buf[32 * 1024];

    if (file_size > sizeof(file_buf)) {
        kernel::serial::write("[efifs]: file too large\n");
        return false;
    }

    uint8_t* write_ptr = file_buf;
    uint16_t fc = static_cast<uint16_t>(file_start_cluster);

    while (fc >= 2 && fc < 0xFFF8u && remaining > 0) {
        const uint32_t sector0 = first_data_sector + (static_cast<uint32_t>(fc) - 2u) * sectors_per_cluster;

        for (uint32_t s = 0; s < sectors_per_cluster && remaining > 0; ++s) {
            if (!read_sectors(sector0 + s, write_ptr, 1)) {
                kernel::serial::write("[efifs]: failed reading file cluster\n");
                return false;
            }

            const uint32_t to_copy = remaining < 512u ? remaining : 512u;
            write_ptr += to_copy;
            remaining -= to_copy;
        }

        fc = next_cluster(fc);
        if (fc == 0xFFFFu) break;
    }

    // Apply seed: parse lines and call simple handlers (md/md supported)
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("[efifs]: ramfs not initialized\n");
        return false;
    }

    char linebuf[128];
    uint32_t line_len = 0;
    for (uint32_t i = 0; i < file_size; ++i) {
        char ch = static_cast<char>(file_buf[i]);

        if (ch == '\r' || ch == '\n') {
            if (line_len > 0) {
                linebuf[line_len] = '\0';
                // apply a minimal subset of seed commands
                char* cursor = linebuf;
                while (*cursor == ' ' || *cursor == '\t') ++cursor;
                if (*cursor != '\0' && *cursor != '#') {
                    char* cmd = cursor;
                    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') ++cursor;
                    if (*cursor != '\0') *cursor++ = '\0';
                    while (*cursor == ' ' || *cursor == '\t') ++cursor;
                    if (*cmd == 'm' && cmd[1] == 'd') {
                        kernel::fs::g_fs->md(cursor);
                    } else if (line_len >= 5 && cmd[0] == 'm' && cmd[1] == 'd') {
                        kernel::fs::g_fs->md(cursor);
                    }
                }
                line_len = 0;
            }
            continue;
        }

        if (line_len + 1 < sizeof(linebuf)) linebuf[line_len++] = ch;
    }

    if (line_len > 0) {
        linebuf[line_len] = '\0';
        // apply last line
        char* cursor = linebuf;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor != '\0' && *cursor != '#') {
            char* cmd = cursor;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') ++cursor;
            if (*cursor != '\0') *cursor++ = '\0';
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (*cmd == 'm' && cmd[1] == 'd') {
                kernel::fs::g_fs->md(cursor);
            }
        }
    }

    kernel::serial::write("[efifs]: ROOTFS.SEED applied from ESP\n");
    return true;
}

} // namespace efifs
} // namespace kernel
