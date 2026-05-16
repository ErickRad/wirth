#include "userland.hpp"
#include "../serial.hpp"
#include <stdarg.h>

namespace kernigham::loader {

void UserBinaryLoader::debug_log(const char* msg) {
    kernel::serial::write(msg);
    kernel::serial::write("\r\n");
}

void UserBinaryLoader::debug_log_fmt(const char* fmt, ...) {
    char buf[512];
    char* out = buf;
    char* end = buf + sizeof(buf) - 1;
    
    va_list ap;
    va_start(ap, fmt);
    
    while (*fmt && out < end) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'X': {
                    uint32_t val = va_arg(ap, uint32_t);
                    const char* digits = "0123456789ABCDEF";
                    char hex[16];
                    int len = 0;
                    if (val == 0) hex[len++] = '0';
                    else {
                        uint32_t v = val;
                        while (v) { hex[len++] = digits[v & 0xF]; v >>= 4; }
                        for (int i = 0; i < len/2; i++) {
                            char tmp = hex[i];
                            hex[i] = hex[len-1-i];
                            hex[len-1-i] = tmp;
                        }
                    }
                    for (int i = 0; i < len && out < end; i++) *out++ = hex[i];
                    break;
                }
                case 'u': {
                    uint32_t val = va_arg(ap, uint32_t);
                    char dec[16];
                    int len = 0;
                    if (val == 0) dec[len++] = '0';
                    else {
                        uint32_t v = val;
                        while (v) { dec[len++] = '0' + (v % 10); v /= 10; }
                        for (int i = 0; i < len/2; i++) {
                            char tmp = dec[i];
                            dec[i] = dec[len-1-i];
                            dec[len-1-i] = tmp;
                        }
                    }
                    for (int i = 0; i < len && out < end; i++) *out++ = dec[i];
                    break;
                }
                case 's': {
                    const char* str = va_arg(ap, const char*);
                    while (*str && out < end) *out++ = *str++;
                    break;
                }
                case '%':
                    *out++ = '%';
                    break;
                default:
                    *out++ = '%';
                    *out++ = *fmt;
            }
            fmt++;
        } else if (*fmt == '\n') {
            *out++ = '\n';
            fmt++;
        } else {
            *out++ = *fmt++;
        }
    }
    
    va_end(ap);
    *out = '\0';
    debug_log(buf);
}

}

