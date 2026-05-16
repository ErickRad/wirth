#pragma once

#include <stdint.h>

namespace kernel::syscall {

enum Number : uint32_t {
    kWrite = 1,
    kExit = 2,
    kSleep = 3,
    kOpen = 4,
    kRead = 5,
    kClose = 6,
    kGetPid = 7,
    kGetTid = 8,
    kProcCount = 9,
    kMkdir = 10,
    kGetUid = 11,
    kGetGid = 12,
    kReaddir = 13,
    kRmdir = 14,
    kUnlink = 15,
};

}  // namespace kernel::syscall

extern "C" uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2);
