#pragma once

#include <stdint.h>

namespace kernel::drivers {

void ide_init();
bool ide_present();
bool ide_read_sectors(uint32_t lba, uint8_t* buf, uint32_t count);
bool ide_write_sectors(uint32_t lba, const uint8_t* buf, uint32_t count);
// Fill `out_buf` with 512 bytes of IDENTIFY DEVICE data. Returns true on success.
bool ide_identify(uint8_t* out_buf);

} // namespace kernel::drivers
