#pragma once

#include <stdint.h>

namespace kernel::pci {

struct Device {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq;
    uint32_t bar[6];
};

uint32_t config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
void config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

// Scans PCI bus and fills up to `max` entries in `out`. Returns true on success and
// writes the number of entries found into `found` (<= max).
bool scan_devices(Device* out, uint32_t max, uint32_t* found);

} // namespace kernel::pci
