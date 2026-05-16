#pragma once

#include <stdint.h>

namespace kernel::ring3 {

// Declaration of assembly function
extern "C" void enter_ring3_simple(uint32_t entry_point, uint32_t user_esp0);

// C++ wrapper
inline void enter_userland(uint32_t entry_point, uint32_t user_esp0) {
    // This function will never return; it jumps to ring3
    enter_ring3_simple(entry_point, user_esp0);
}

}
