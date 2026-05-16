#pragma once

#include <stdint.h>

namespace kernel::arch::x86::pic {

void remap(uint8_t master_offset, uint8_t slave_offset);
void set_irq_mask(uint8_t irq_line);
void clear_irq_mask(uint8_t irq_line);
void send_eoi(uint8_t irq);

}  // namespace kernel::arch::x86::pic
