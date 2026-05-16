#include <stddef.h>
#include <stdint.h>

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

extern "C" void* memset(void* dst, int value, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t byte = static_cast<uint8_t>(value);
    for (size_t i = 0; i < n; ++i) {
        d[i] = byte;
    }
    return dst;
}
