#pragma once

#include <stdint.h>

namespace kernel {
namespace usbms {

bool init();
bool present();
bool read_sectors(uint64_t lba, uint8_t* buf, uint32_t count);

} // namespace usbms
} // namespace kernel
