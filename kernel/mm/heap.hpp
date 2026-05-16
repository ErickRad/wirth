#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm::heap {

void init(uint32_t heap_start, uint32_t heap_limit);
void* alloc(size_t size, size_t alignment = 8);
uint32_t mapped_bytes();
uint32_t used_bytes();

}  // namespace kernel::mm::heap
