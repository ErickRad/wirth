#pragma once

#include <stdint.h>

namespace kernel::input::keyboard {

// Initialize keyboard subsystem (noop for now)
void init();

// Drain PS/2 controller into internal buffer (polling helper)
void drain_poll();

// Enqueue a scancode-derived character from ISR context (non-blocking)
void enqueue_from_isr(uint8_t scancode);

// Dequeue next character (non-blocking). Returns true if a char was available.
bool dequeue_char(char* out);

} // namespace kernel::input::keyboard
