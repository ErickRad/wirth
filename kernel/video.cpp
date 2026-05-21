// Keep a single, consolidated implementation (minimal ANSI support)
#include "video.hpp"

#include <stdint.h>

namespace kernel {
namespace video {

static volatile uint16_t* const kVga = reinterpret_cast<uint16_t*>(0xB8000);
static const uint8_t kAttr = 0x07; // light grey on black
static const int kCols = 80;
static const int kRows = 25;
static int g_row = 0;
static int g_col = 0;

static void scroll_up() {

    for (int r = 1; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            kVga[(r - 1) * kCols + c] = kVga[r * kCols + c];
        }
    }

    // clear last line
    for (int c = 0; c < kCols; ++c) {
        kVga[(kRows - 1) * kCols + c] = (uint16_t)(' ' | (kAttr << 8));
    }

    if (g_row > 0) {
        g_row = kRows - 1;
    }
}

void init() {

    clear();

}

void clear() {

    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            kVga[r * kCols + c] = (uint16_t)(' ' | (kAttr << 8));
        }
    }

    g_row = 0;
    g_col = 0;

}

void write_char(char ch) {
    if (ch == '\r') {

        g_col = 0;
        return;
    }

    if (ch == '\n') {

        g_col = 0;
        ++g_row;

        if (g_row >= kRows) {
            scroll_up();
        }

        return;
    }

    if (ch == '\b') {

        if (g_col > 0) {
            --g_col;
            kVga[g_row * kCols + g_col] = (uint16_t)(' ' | (kAttr << 8));
        }

        return;
    }

    if (ch < 32 || ch > 126) {
        return;
    }

    kVga[g_row * kCols + g_col] = (uint16_t)((uint8_t)ch | (kAttr << 8));
    ++g_col;

    if (g_col >= kCols) {

        g_col = 0;
        ++g_row;

        if (g_row >= kRows) {
            scroll_up();
        }
    }
}

// Minimal ANSI-ish sequence support: handle "\x1b[2J" (clear) and "\x1b[H" (home) and "\x1b[2K" (clear line)
void write(const char* s) {
    if (s == nullptr) return;

    for (uint32_t i = 0; s[i] != '\0'; ++i) {

        char c = s[i];

        if (c == '\x1b' && s[i+1] == '[') {
            uint32_t j = i + 2;

            // parse simple numeric parameter

            int num = 0;
            bool has_num = false;

            while (s[j] >= '0' && s[j] <= '9') { has_num = true; num = num * 10 + (s[j] - '0'); ++j; }

            char cmd = s[j];

            if (cmd == 'J' && has_num && num == 2) {

                clear();
                i = j; // skip sequence

                continue;
            }

            if (cmd == 'K' && has_num && num == 2) {

                // clear current line
                for (int col = 0; col < kCols; ++col) {
                    kVga[g_row * kCols + col] = (uint16_t)(' ' | (kAttr << 8));
                }

                g_col = 0;
                i = j;

                continue;
            }

            if (cmd == 'H') {

                g_row = 0;
                g_col = 0;
                i = j;

                continue;
            }
            continue;
        }

        write_char(c);
    }
}

} // namespace video
} // namespace kernel
