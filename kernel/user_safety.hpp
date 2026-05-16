#pragma once

#include <stdint.h>

namespace kernel::user_safety {

// Check if a pointer is accessible in user space (ring3)
// Returns true if the pointer is safe to dereference from kernel
bool is_user_pointer(uint32_t ptr);

// Check if a buffer [ptr, ptr+size) is entirely in user space
bool is_user_buffer(uint32_t ptr, uint32_t size);

// Validate string pointer (must be null-terminated in user space)
bool is_user_string(uint32_t ptr, uint32_t max_len);

}  // namespace kernel::user_safety
