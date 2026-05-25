#include "keyboard.hpp"

#include "../arch/x86/io.hpp"
#include "../serial.hpp"

namespace {
    static char g_kbuf[256];
    static unsigned g_head = 0;
    static unsigned g_tail = 0;

    inline unsigned next(unsigned i) { return (i + 1) & (sizeof(g_kbuf) - 1); }
}

namespace kernel::input::keyboard {

void init() {
    // nothing yet
}

void drain_poll() {
    while ((kernel::arch::x86::io::inb(0x64) & 0x01u) != 0) {
        const uint8_t sc = kernel::arch::x86::io::inb(0x60);
        if (sc & 0x80u) continue;

        enqueue_from_isr(sc);
    }
}

void enqueue_from_isr(uint8_t scancode) {
    char ch = 0;
    switch (scancode) {
        case 0x1C: ch='\n'; break;
        case 0x0E: ch='\b'; break;
        case 0x39: ch=' '; break;
        case 0x02: ch='1'; break; case 0x03: ch='2'; break; case 0x04: ch='3'; break;
        case 0x05: ch='4'; break; case 0x06: ch='5'; break; case 0x07: ch='6'; break;
        case 0x08: ch='7'; break; case 0x09: ch='8'; break; case 0x0A: ch='9'; break;
        case 0x0B: ch='0'; break;
        case 0x10: ch='q'; break; case 0x11: ch='w'; break; case 0x12: ch='e'; break;
        case 0x13: ch='r'; break; case 0x14: ch='t'; break; case 0x15: ch='y'; break;
        case 0x16: ch='u'; break; case 0x17: ch='i'; break; case 0x18: ch='o'; break;
        case 0x19: ch='p'; break;
        case 0x1E: ch='a'; break; case 0x1F: ch='s'; break; case 0x20: ch='d'; break;
        case 0x21: ch='f'; break; case 0x22: ch='g'; break; case 0x23: ch='h'; break;
        case 0x24: ch='j'; break; case 0x25: ch='k'; break; case 0x26: ch='l'; break;
        case 0x2C: ch='z'; break; case 0x2D: ch='x'; break; case 0x2E: ch='c'; break;
        case 0x2F: ch='v'; break; case 0x30: ch='b'; break; case 0x31: ch='n'; break;
        case 0x32: ch='m'; break; case 0x33: ch=','; break; case 0x34: ch='.'; break;
        case 0x35: ch='/'; break; case 0x27: ch=';'; break; case 0x28: ch='\''; break;
        case 0x29: ch='`'; break; case 0x1A: ch='['; break; case 0x1B: ch=']'; break;
        case 0x2B: ch='\\'; break; case 0x0C: ch='-'; break; case 0x0D: ch='='; break;
        default: ch = 0; break;
    }

    if (ch == 0) return;

    const unsigned n = next(g_head);
    if (n == g_tail) return; // buffer full, drop
    g_kbuf[g_head] = ch;
    g_head = n;
}

bool dequeue_char(char* out) {
    if (g_head == g_tail) return false;
    *out = g_kbuf[g_tail];
    g_tail = next(g_tail);
    return true;
}

} // namespace kernel::input::keyboard
