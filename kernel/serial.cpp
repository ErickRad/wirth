#include "serial.hpp"

#include "arch/x86/io.hpp"

namespace {

constexpr uint16_t kCom1Port = 0x3F8;
bool g_drop_current_line = false;

bool starts_with_kernigham_tag(const char* text) {
    if (text == nullptr) {
        return false;
    }
    return text[0] == '[' && text[1] == 'h' && text[2] == 'o' && text[3] == 'k' &&
           text[4] == 'u' && text[5] == 's' && text[6] == 'a' && text[7] == 'i';
}

}  // namespace

namespace kernel::serial {

void init() {
    arch::x86::io::outb(kCom1Port + 1, 0x00);
    arch::x86::io::outb(kCom1Port + 3, 0x80);
    arch::x86::io::outb(kCom1Port + 0, 0x03);
    arch::x86::io::outb(kCom1Port + 1, 0x00);
    arch::x86::io::outb(kCom1Port + 3, 0x03);
    arch::x86::io::outb(kCom1Port + 2, 0xC7);
    arch::x86::io::outb(kCom1Port + 4, 0x0B);
}

void write_char(char c) {
    if (g_drop_current_line) {
        if (c == '\n' || c == '\r') {
            g_drop_current_line = false;
        }
        return;
    }
    arch::x86::io::outb(kCom1Port, static_cast<uint8_t>(c));
}

void write(const char* text) {
    if (text == nullptr) {
        return;
    }
    if (starts_with_kernigham_tag(text)) {
        g_drop_current_line = true;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        write_char(*p);
    }
}

void write_hex(uint32_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        write_char(digits[(value >> shift) & 0xF]);
    }
}

void write_hex64(uint64_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4) {
        write_char(digits[(value >> shift) & 0xF]);
    }
}

bool read_char_nonblocking(char* out_char) {
    if (out_char == nullptr) {
        return false;
    }
    const uint8_t lsr = arch::x86::io::inb(kCom1Port + 5);
    if ((lsr & 0x01u) == 0) {
        return false;
    }
    *out_char = static_cast<char>(arch::x86::io::inb(kCom1Port + 0));
    return true;
}

char read_char_blocking() {
    char c = 0;
    while (!read_char_nonblocking(&c)) {
        asm volatile("hlt");
    }
    return c;
}

}  // namespace kernel::serial
