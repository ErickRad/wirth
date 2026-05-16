#pragma once

#include <stdint.h>

namespace kernel::mm::pmm {

void init(uint32_t multiboot_info_addr, uint32_t kernel_start, uint32_t kernel_end);
uint32_t alloc_frame();
void free_frame(uint32_t phys_addr);
uint32_t total_frames();
uint32_t free_frames();

}  // namespace kernel::mm::pmm
