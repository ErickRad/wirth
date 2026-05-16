#include "pit.hpp"

#include "io.hpp"

namespace {

constexpr uint32_t kPitInputHz = 1193182;
constexpr uint16_t kPitChannel0 = 0x40;
constexpr uint16_t kPitCommand = 0x43;
constexpr uint8_t kCmdRateGeneratorLoHi = 0x36;

}  // namespace

namespace kernel::arch::x86::pit {

void init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }
    uint32_t divisor = kPitInputHz / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    io::outb(kPitCommand, kCmdRateGeneratorLoHi);
    io::outb(kPitChannel0, static_cast<uint8_t>(divisor & 0xFF));
    io::outb(kPitChannel0, static_cast<uint8_t>((divisor >> 8) & 0xFF));
}

}  // namespace kernel::arch::x86::pit
