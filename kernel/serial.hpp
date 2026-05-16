#pragma once

#include <stdint.h>

namespace kernel::serial {

void init();
void write_char(char c);
void write(const char* text);
void write_hex(uint32_t value);
void write_hex64(uint64_t value);
bool read_char_nonblocking(char* out_char);
char read_char_blocking();

}  // namespace kernel::serial
