#include "user_safety.hpp"

#include "mm/vmm.hpp"

namespace {

// User space is typically low addresses (not kernel-reserved ranges)
// Kernel: 0xC0000000+ (and identity map at 0x00000000-0x04000000)
// User: 0x08000000-0xBFFFFFFF
constexpr uint32_t kUserSpaceStart = 0x08000000u;
constexpr uint32_t kUserSpaceEnd = 0xC0000000u;

}  // namespace

namespace kernel::user_safety {

bool is_user_pointer(uint32_t ptr) {
    return ptr >= kUserSpaceStart && ptr < kUserSpaceEnd;
}

bool is_user_buffer(uint32_t ptr, uint32_t size) {
    if (size == 0) {
        return false;
    }
    if (ptr < kUserSpaceStart) {
        return false;
    }
    const uint32_t end = ptr + size;
    if (end < ptr) {
        return false;
    }
    if (end > kUserSpaceEnd) {
        return false;
    }
    return true;
}

bool is_user_string(uint32_t ptr, uint32_t max_len) {
    if (!is_user_pointer(ptr)) {
        return false;
    }
    const char* str = reinterpret_cast<const char*>(ptr);
    for (uint32_t i = 0; i < max_len; ++i) {
        if (str[i] == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace kernel::user_safety
