#include "framebuffer.hpp"

#include <stdint.h>
#include <stddef.h>

#include "boot/multiboot2.hpp"
#include "serial.hpp"

namespace kernel::framebuffer {

static Info g_info = {nullptr,0,0,0,0,false};

// Minimal 8x8 font (public domain "font8x8_basic") for ASCII 32..127
static const uint8_t kFont8x8[96][8] = {
    // space .. ~ (only a small subset shown for brevity; render unknowns as blank)
    {0,0,0,0,0,0,0,0}, // 32 ' '
    {0,0,94,0,0,0,0,0}, // 33 '!'
    {20,8,0,0,0,0,0,0},
    {36,126,36,126,36,0,0,0},
    {0}, // rest default to zeros
};

static inline void plot_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_info.available) return;
    if (x >= g_info.width || y >= g_info.height) return;
    uint8_t* fb = reinterpret_cast<uint8_t*>(g_info.addr);
    uint8_t* px = fb + y * g_info.pitch + x * (g_info.bpp / 8);
    if (g_info.bpp == 32) {
        *reinterpret_cast<uint32_t*>(px) = color;
    } else if (g_info.bpp == 24) {
        px[0] = color & 0xFF;
        px[1] = (color >> 8) & 0xFF;
        px[2] = (color >> 16) & 0xFF;
    }
}

void init_manual(void* addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp) {
    g_info.addr = addr;
    g_info.width = width;
    g_info.height = height;
    g_info.pitch = pitch;
    g_info.bpp = bpp;
    g_info.available = (addr != nullptr && width > 0 && height > 0 && pitch > 0 && bpp >= 24);
}

void init_from_multiboot(uint32_t multiboot_info_addr) {
    // parse multiboot2 tags for framebuffer (tag type 8)
    const uint8_t* info = reinterpret_cast<const uint8_t*>(multiboot_info_addr);
    if (info == nullptr) return;
    const uint32_t total_size = *reinterpret_cast<const uint32_t*>(info);
    const uintptr_t info_begin = reinterpret_cast<uintptr_t>(info);
    const uintptr_t info_end = info_begin + total_size;

    struct Tag { uint32_t type; uint32_t size; };

    uintptr_t cursor = info_begin + 8;

    while (cursor + sizeof(Tag) <= info_end) {
        const Tag* tag = reinterpret_cast<const Tag*>(cursor);
        if (tag->type == 0) break;
        if (tag->type == 8) {
            // framebuffer tag
            const uint8_t* t = reinterpret_cast<const uint8_t*>(cursor);
            // layout per multiboot2: addr (u64), pitch (u32), width(u32), height(u32), bpp(u8), type(u8), reserved[2]
            const uint64_t fb_addr = *reinterpret_cast<const uint64_t*>(t + 8);
            const uint32_t pitch = *reinterpret_cast<const uint32_t*>(t + 16);
            const uint32_t width = *reinterpret_cast<const uint32_t*>(t + 20);
            const uint32_t height = *reinterpret_cast<const uint32_t*>(t + 24);
            const uint8_t bpp = *(t + 28);

            init_manual(reinterpret_cast<void*>(static_cast<uintptr_t>(fb_addr)), width, height, pitch, bpp);
            kernel::serial::write("[fb] framebuffer detected\n");
            return;
        }

        cursor = (cursor + tag->size + 7) & ~static_cast<uintptr_t>(7);
    }

    // not found
}

// Simple character rendering using 8x8 font; maps characters into 8x8 blocks
static uint32_t g_fg = 0x00FFFFFFu; // white
static uint32_t g_bg = 0x00000000u; // black

void clear() {
    if (!g_info.available) return;
    uint8_t* fb = reinterpret_cast<uint8_t*>(g_info.addr);
    for (uint32_t y = 0; y < g_info.height; ++y) {
        for (uint32_t x = 0; x < g_info.pitch; ++x) {
            fb[y * g_info.pitch + x] = 0;
        }
    }
}

static uint32_t cursor_x = 0, cursor_y = 0;
static const uint32_t cell_w = 8;
static const uint32_t cell_h = 8;

void write_char(char ch) {
    if (!g_info.available) return;
    if (ch == '\n') {
        cursor_x = 0;
        cursor_y += cell_h;
        if (cursor_y + cell_h > g_info.height) cursor_y = 0;
        return;
    }
    if (ch == '\r') { cursor_x = 0; return; }

    const unsigned idx = (ch >= 32) ? (ch - 32) : 0;
    const uint8_t* glyph = kFont8x8[idx];

    for (uint32_t row = 0; row < cell_h; ++row) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < cell_w; ++col) {
            uint32_t px = cursor_x + col;
            uint32_t py = cursor_y + row;
            if (bits & (1 << (7 - col))) {
                plot_pixel(px, py, g_fg);
            } else {
                plot_pixel(px, py, g_bg);
            }
        }
    }

    cursor_x += cell_w;
    if (cursor_x + cell_w > g_info.width) {
        cursor_x = 0;
        cursor_y += cell_h;
        if (cursor_y + cell_h > g_info.height) cursor_y = 0;
    }
}

void write(const char* s) {
    if (s == nullptr) return;
    for (const char* p = s; *p != '\0'; ++p) write_char(*p);
}

const Info& info() { return g_info; }

} // namespace kernel::framebuffer
