#pragma once

#include <stdint.h>

namespace kernel::arch {

#if defined(KERNEL_ARCH_X86_64)
namespace active = x86_64;
#elif defined(KERNEL_ARCH_X86)
namespace active = x86;
#else
namespace active = x86;
#endif

}  // namespace kernel::arch
