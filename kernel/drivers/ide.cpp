#include "ide.hpp"
#include "../arch/x86/io.hpp"
#include "../serial.hpp"

namespace io = kernel::arch::x86::io;

namespace kernel::drivers {

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v) {
    asm volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

enum {
    ATA_DATA = 0x1F0,
    ATA_ERROR = 0x1F1,
    ATA_SECTOR_COUNT = 0x1F2,
    ATA_LBA_LOW = 0x1F3,
    ATA_LBA_MID = 0x1F4,
    ATA_LBA_HIGH = 0x1F5,
    ATA_DRIVE = 0x1F6,
    ATA_STATUS = 0x1F7,
    ATA_COMMAND = 0x1F7,
    ATA_CONTROL = 0x3F6
};

static void ata_delay() {
    io::outb(0x80, 0);
}

static uint8_t ata_status() {
    return io::inb(ATA_STATUS);
}

void ide_init() {
    // Disable interrupts on primary ATA controller
    io::outb(ATA_CONTROL, 0);
    serial::write("ide: init\n");
}

bool ide_present() {
    // Try IDENTIFY
    io::outb(ATA_DRIVE, 0xA0); // master
    ata_delay();
    io::outb(ATA_SECTOR_COUNT, 0);
    io::outb(ATA_LBA_LOW, 0);
    io::outb(ATA_LBA_MID, 0);
    io::outb(ATA_LBA_HIGH, 0);
    io::outb(ATA_COMMAND, 0xEC);
    ata_delay();

    uint8_t st = ata_status();
    if (st == 0) return false;
    // wait for BSY clear
    while (st & 0x80) st = ata_status();

    // If ERR set, no identify
    if (st & 0x01) return false;
    return true;
}

bool ide_read_sectors(uint32_t lba, uint8_t* buf, uint32_t count) {
    for (uint32_t s = 0; s < count; ++s) {
        uint32_t cur = lba + s;
        io::outb(ATA_DRIVE, 0xE0 | ((cur >> 24) & 0x0F));
        io::outb(ATA_SECTOR_COUNT, 1);
        io::outb(ATA_LBA_LOW, (uint8_t)(cur & 0xFF));
        io::outb(ATA_LBA_MID, (uint8_t)((cur >> 8) & 0xFF));
        io::outb(ATA_LBA_HIGH, (uint8_t)((cur >> 16) & 0xFF));
        io::outb(ATA_COMMAND, 0x20); // READ SECTORS

        // wait for BSY clear
        uint8_t st = ata_status();
        while (st & 0x80) st = ata_status();
        // wait for DRQ
        while (!(st & 0x08)) st = ata_status();

        // read 256 words
        for (int i = 0; i < 256; ++i) {
            uint16_t w = inw(ATA_DATA);
            buf[0] = w & 0xFF; buf[1] = (w >> 8) & 0xFF;
            buf += 2;
        }
    }
    return true;
}

bool ide_write_sectors(uint32_t lba, const uint8_t* buf, uint32_t count) {
    for (uint32_t s = 0; s < count; ++s) {
        uint32_t cur = lba + s;
        io::outb(ATA_DRIVE, 0xE0 | ((cur >> 24) & 0x0F));
        io::outb(ATA_SECTOR_COUNT, 1);
        io::outb(ATA_LBA_LOW, (uint8_t)(cur & 0xFF));
        io::outb(ATA_LBA_MID, (uint8_t)((cur >> 8) & 0xFF));
        io::outb(ATA_LBA_HIGH, (uint8_t)((cur >> 16) & 0xFF));
        io::outb(ATA_COMMAND, 0x30); // WRITE SECTORS

        // wait for BSY clear
        uint8_t st = ata_status();
        while (st & 0x80) st = ata_status();
        // wait for DRQ
        while (!(st & 0x08)) st = ata_status();

        // write 256 words
        for (int i = 0; i < 256; ++i) {
            uint16_t w = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            outw(ATA_DATA, w);
            buf += 2;
        }

        // flush
        io::outb(ATA_COMMAND, 0xE7); // FLUSH CACHE
    }
    return true;
}

} // namespace kernel::drivers
