#include "usb_mass_storage.hpp"

#include "xhci.hpp"
#include "serial.hpp"
#include "block.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kCbwSignature = 0x43425355u; // 'USBC'
constexpr uint32_t kCswSignature = 0x53425355u; // 'USBS'

struct Cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} __attribute__((packed));

struct Csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed));

static uint32_t g_tag = 1;

static bool scsi_xfer(const kernel::xhci::Device& dev, const uint8_t* cdb, uint8_t cdb_len,
                      void* data, uint32_t data_len, bool dir_in) {
    Cbw cbw = {};
    cbw.signature = kCbwSignature;
    cbw.tag = g_tag++;

    cbw.data_len = data_len;
    cbw.flags = dir_in ? 0x80u : 0x00u;
    cbw.lun = 0;
    
    cbw.cb_len = cdb_len;

    for (uint8_t i = 0; i < cdb_len && i < sizeof(cbw.cb); ++i) {
        cbw.cb[i] = cdb[i];
    }

    if (!kernel::xhci::bulk_transfer(dev, dev.bulk_out, &cbw, sizeof(cbw))) {
        kernel::serial::write("usbms: CBW send failed\n");
        return false;
    }

    if (data_len > 0) {
        if (dir_in) {
            if (!kernel::xhci::bulk_transfer(dev, dev.bulk_in, data, data_len)) {
                kernel::serial::write("usbms: data IN failed\n");
                return false;
            }
        } else {
            if (!kernel::xhci::bulk_transfer(dev, dev.bulk_out, data, data_len)) {
                kernel::serial::write("usbms: data OUT failed\n");
                return false;
            }
        }
    }

    Csw csw = {};
    if (!kernel::xhci::bulk_transfer(dev, dev.bulk_in, &csw, sizeof(csw))) {
        kernel::serial::write("usbms: CSW read failed\n");
        return false;
    }

    if (csw.signature != kCswSignature || csw.tag != cbw.tag || csw.status != 0) {
        kernel::serial::write("usbms: CSW invalid\n");
        return false;
    }

    return true;
}

} // namespace

namespace kernel {
namespace usbms {

static bool g_initialized = false;
static kernel::xhci::Device g_dev = {};
static uint32_t g_block_size = 512;
static uint64_t g_block_count = 0;

bool init() {
    if (!kernel::xhci::init()) {
        kernel::serial::write("usbms: xHCI init failed\n");
        return false;
    }

    if (!kernel::xhci::get_mass_storage_device(&g_dev)) {
        kernel::serial::write("usbms: no mass-storage device\n");
        return false;
    }

    uint8_t cdb[10] = {};
    uint8_t cap[8] = {};
    
    cdb[0] = 0x25; // READ CAPACITY (10)

    if (scsi_xfer(g_dev, cdb, 10, cap, sizeof(cap), true)) {
    
        const uint32_t last_lba = (static_cast<uint32_t>(cap[0]) << 24) |
                                  (static_cast<uint32_t>(cap[1]) << 16) |
                                  (static_cast<uint32_t>(cap[2]) << 8) |
                                  (static_cast<uint32_t>(cap[3]));
    
        g_block_size = (static_cast<uint32_t>(cap[4]) << 24) |
                       (static_cast<uint32_t>(cap[5]) << 16) |
                       (static_cast<uint32_t>(cap[6]) << 8) |
                       (static_cast<uint32_t>(cap[7]));
    
        g_block_count = static_cast<uint64_t>(last_lba) + 1u;
    }

    if (g_block_size == 0) g_block_size = 512;

    g_initialized = true;
    kernel::serial::write("usbms: initialized\n");

    // Register as block device
    static kernel::BlockDevice usb_block = {};

    auto usb_read_wrapper = [](uint64_t lba, uint8_t* buf, uint32_t count) -> bool {
        // Use scsi_xfer to read in chunks
        uint32_t remaining = count;
        uint8_t* out = buf;
        uint64_t cur = lba;
        while (remaining > 0) {
            uint32_t chunk = (remaining > 32u) ? 32u : remaining;
            uint8_t cdb[10] = {0x28,0,0,0,0,0,0,0,0,0};
            if (!scsi_xfer(g_dev, cdb, 10, out, chunk * g_block_size, true)) {
                return false;
            }
            remaining -= chunk;
            cur += chunk;
            out += chunk * g_block_size;
        }
        return true;
    };

    usb_block.name = "usbms0";
    usb_block.block_size = g_block_size;
    usb_block.block_count = g_block_count;
    usb_block.read_sectors = reinterpret_cast<bool (*)(uint64_t, uint8_t*, uint32_t)>(+usb_read_wrapper);
    auto usb_write_wrapper = [](uint64_t lba, const uint8_t* buf, uint32_t count) -> bool {
        // perform SCSI WRITE(10) in chunks of up to 32 sectors
        uint32_t remaining = count;
        const uint8_t* in = buf;
        uint64_t cur = lba;
        while (remaining > 0) {
            uint32_t chunk = (remaining > 32u) ? 32u : remaining;
            uint8_t cdb[10] = {};
            cdb[0] = 0x2A; // WRITE(10)
            cdb[2] = static_cast<uint8_t>((cur >> 24) & 0xFFu);
            cdb[3] = static_cast<uint8_t>((cur >> 16) & 0xFFu);
            cdb[4] = static_cast<uint8_t>((cur >> 8) & 0xFFu);
            cdb[5] = static_cast<uint8_t>(cur & 0xFFu);
            cdb[7] = static_cast<uint8_t>((chunk >> 8) & 0xFFu);
            cdb[8] = static_cast<uint8_t>(chunk & 0xFFu);

            if (!scsi_xfer(g_dev, cdb, 10, const_cast<uint8_t*>(in), chunk * g_block_size, false)) {
                return false;
            }

            remaining -= chunk;
            cur += chunk;
            in += chunk * g_block_size;
        }

        return true;
    };

    usb_block.write_sectors = reinterpret_cast<bool (*)(uint64_t, const uint8_t*, uint32_t)>(+usb_write_wrapper);
    usb_block.ctx = nullptr;

    (void)kernel::block_register_device(&usb_block);

    return true;
}

bool present() {
    return g_initialized;
}

bool read_sectors(uint64_t lba, uint8_t* buf, uint32_t count) {
    
    if (!g_initialized || buf == nullptr || count == 0) return false;
    
    if (g_block_size != 512) {
        kernel::serial::write("usbms: unsupported block size\n");
        return false;
    }

    const uint32_t max_sectors = 32;
    
    uint32_t remaining = count;
    uint8_t* out = buf;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        const uint32_t chunk = (remaining > max_sectors) ? max_sectors : remaining;
    
        uint8_t cdb[10] = {};
    
        cdb[0] = 0x28; // READ(10)
        cdb[2] = static_cast<uint8_t>((cur_lba >> 24) & 0xFFu);
        cdb[3] = static_cast<uint8_t>((cur_lba >> 16) & 0xFFu);
        cdb[4] = static_cast<uint8_t>((cur_lba >> 8) & 0xFFu);
    
        cdb[5] = static_cast<uint8_t>(cur_lba & 0xFFu);
        cdb[7] = static_cast<uint8_t>((chunk >> 8) & 0xFFu);
        cdb[8] = static_cast<uint8_t>(chunk & 0xFFu);

        const uint32_t bytes = chunk * 512u;
    
        if (!scsi_xfer(g_dev, cdb, 10, out, bytes, true)) {
            return false;
        }

        remaining -= chunk;
        cur_lba += chunk;
        out += bytes;
    }

    return true;
}

} // namespace usbms
} // namespace kernel
