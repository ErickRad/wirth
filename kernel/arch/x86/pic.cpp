#include "pic.hpp"

#include "io.hpp"

namespace {

constexpr uint16_t kPic1Cmd = 0x20;
constexpr uint16_t kPic1Data = 0x21;
constexpr uint16_t kPic2Cmd = 0xA0;
constexpr uint16_t kPic2Data = 0xA1;

constexpr uint8_t kIcw1Init = 0x10;
constexpr uint8_t kIcw1Icw4 = 0x01;
constexpr uint8_t kIcw4_8086 = 0x01;
constexpr uint8_t kPicEoi = 0x20;

}  // namespace

namespace kernel::arch::x86::pic {

void remap(uint8_t master_offset, uint8_t slave_offset) {
    const uint8_t master_mask = io::inb(kPic1Data);
    const uint8_t slave_mask = io::inb(kPic2Data);

    io::outb(kPic1Cmd, kIcw1Init | kIcw1Icw4);
    io::io_wait();
    io::outb(kPic2Cmd, kIcw1Init | kIcw1Icw4);
    io::io_wait();

    io::outb(kPic1Data, master_offset);
    io::io_wait();
    io::outb(kPic2Data, slave_offset);
    io::io_wait();

    io::outb(kPic1Data, 4);
    io::io_wait();
    io::outb(kPic2Data, 2);
    io::io_wait();

    io::outb(kPic1Data, kIcw4_8086);
    io::io_wait();
    io::outb(kPic2Data, kIcw4_8086);
    io::io_wait();

    io::outb(kPic1Data, master_mask);
    io::outb(kPic2Data, slave_mask);
}

void set_irq_mask(uint8_t irq_line) {
    uint16_t port = kPic1Data;
    if (irq_line >= 8) {
        port = kPic2Data;
        irq_line -= 8;
    }
    const uint8_t value = io::inb(port) | static_cast<uint8_t>(1u << irq_line);
    io::outb(port, value);
}

void clear_irq_mask(uint8_t irq_line) {
    uint16_t port = kPic1Data;
    if (irq_line >= 8) {
        port = kPic2Data;
        irq_line -= 8;
    }
    const uint8_t value = io::inb(port) & static_cast<uint8_t>(~(1u << irq_line));
    io::outb(port, value);
}

void send_eoi(uint8_t irq) {
    if (irq >= 8) {
        io::outb(kPic2Cmd, kPicEoi);
    }
    io::outb(kPic1Cmd, kPicEoi);
}

}  // namespace kernel::arch::x86::pic
