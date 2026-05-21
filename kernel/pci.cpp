#include "pci.hpp"

#include "arch/x86/io.hpp"

namespace {

constexpr uint16_t kPciConfigAddress = 0xCF8;
constexpr uint16_t kPciConfigData = 0xCFC;

uint32_t make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000u | (static_cast<uint32_t>(bus) << 16) |
           (static_cast<uint32_t>(slot) << 11) | (static_cast<uint32_t>(func) << 8) |
           (static_cast<uint32_t>(offset) & 0xFCu);
}

} // namespace

namespace kernel::pci {

uint32_t config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t addr = make_address(bus, slot, func, offset);
    kernel::arch::x86::io::outl(kPciConfigAddress, addr);
    return kernel::arch::x86::io::inl(kPciConfigData);
}

uint16_t config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t val = config_read32(bus, slot, func, offset & 0xFCu);
    const uint32_t shift = (offset & 3u) * 8u;
    return static_cast<uint16_t>((val >> shift) & 0xFFFFu);
}

uint8_t config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t val = config_read32(bus, slot, func, offset & 0xFCu);
    const uint32_t shift = (offset & 3u) * 8u;
    return static_cast<uint8_t>((val >> shift) & 0xFFu);
}

void config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    const uint32_t addr = make_address(bus, slot, func, offset);
    kernel::arch::x86::io::outl(kPciConfigAddress, addr);
    kernel::arch::x86::io::outl(kPciConfigData, value);
}

void config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    const uint32_t aligned = offset & 0xFCu;
    const uint32_t shift = (offset & 3u) * 8u;
    uint32_t cur = config_read32(bus, slot, func, static_cast<uint8_t>(aligned));
    cur &= ~(0xFFFFu << shift);
    cur |= (static_cast<uint32_t>(value) << shift);
    config_write32(bus, slot, func, static_cast<uint8_t>(aligned), cur);
}

bool scan_devices(Device* out, uint32_t max, uint32_t* found) {
    if (found == nullptr) return false;
    *found = 0;

    if (out == nullptr || max == 0) return true;

    uint32_t added = 0;

    for (uint32_t bus = 0; bus < 256u; ++bus) {
        for (uint32_t slot = 0; slot < 32u; ++slot) {
            // read vendor id of function 0
            const uint16_t vendor0 = config_read16(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), 0, 0x00);

            if (vendor0 == 0xFFFFu) {
                continue;
            }

            const uint8_t hdr = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), 0, 0x0E);
            const bool multi = (hdr & 0x80u) != 0;

            const uint32_t fn_max = multi ? 8u : 1u;

            for (uint32_t func = 0; func < fn_max; ++func) {
                const uint16_t vendor = config_read16(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x00);

                if (vendor == 0xFFFFu) continue;

                kernel::pci::Device dev{};

                dev.bus = static_cast<uint8_t>(bus);
                dev.slot = static_cast<uint8_t>(slot);
                dev.func = static_cast<uint8_t>(func);
                dev.vendor_id = vendor;
                dev.device_id = config_read16(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x02);
                dev.prog_if = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x09);
                dev.subclass = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x0A);
                dev.class_code = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x0B);
                dev.header_type = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x0E);
                dev.irq = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), 0x3C);

                for (int i = 0; i < 6; ++i) {
                    dev.bar[i] = config_read32(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(func), static_cast<uint8_t>(0x10 + i * 4));
                }

                if (added < max) {
                    out[added] = dev;
                }

                ++added;

                if (added >= max) {
                    *found = added;
                    return true;
                }
            }
        }
    }

    *found = added;
    return true;
}

} // namespace kernel::pci
