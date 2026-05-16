#pragma once

#include <stdint.h>

namespace kernel::arch::x86::pit {

void init(uint32_t frequency_hz);

}  // namespace kernel::arch::x86::pit
