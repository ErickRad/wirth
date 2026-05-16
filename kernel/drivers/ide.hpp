#pragma once

#include <stdint.h>

namespace kernel::drivers {

void ide_init();
bool ide_present();
bool ide_read_sectors(uint32_t lba, uint8_t* buf, uint32_t count);
bool ide_write_sectors(uint32_t lba, const uint8_t* buf, uint32_t count);

} // namespace kernel::drivers
