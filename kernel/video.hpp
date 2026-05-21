#pragma once

#include <stdint.h>

namespace kernel::video {

void init();
void clear();
void write_char(char c);
void write(const char* s);

} // namespace kernel::video
#pragma once

#include <stdint.h>

namespace kernel {
namespace video {

void init();
void write(const char* s);
void write_char(char c);
void clear();

} // namespace video
} // namespace kernel
