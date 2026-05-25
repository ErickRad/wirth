#pragma once

#include <stdint.h>

namespace kernel::framebuffer {

struct Info {
    void* addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    bool available;
};

// Initialize framebuffer from multiboot2 info pointer (MBI addr)
void init_from_multiboot(uint32_t multiboot_info_addr);

void init_manual(void* addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp);

void write(const char* s);
void write_char(char c);
void clear();

const Info& info();

} // namespace kernel::framebuffer
