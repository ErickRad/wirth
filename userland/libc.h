// Minimal userland libc for ring3 processes
// Syscall interface for userland

#pragma once

#include <stdint.h>

namespace userland {

enum class Syscall : uint32_t {
    kWrite = 1,
    kExit = 2,
    kSleep = 3,
    kOpen = 4,
    kRead = 5,
    kClose = 6,
    kGetPid = 7,
    kGetTid = 8,
    kProcCount = 9,
    kmd = 10,
    kGetUid = 11,
    kGetGid = 12,
    kReaddir = 13,
    krd = 14,
    kUnlink = 15,
};

inline uint32_t syscall(Syscall num, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0) {
    uint32_t ret;
    asm volatile(
        "int $0x80"
        : "=a" (ret)
        : "a" (static_cast<uint32_t>(num)), "b" (a0), "c" (a1), "d" (a2)
        : "memory"
    );
    return ret;
}

inline void sys_exit(uint32_t code) {
    syscall(Syscall::kExit, code);
    while (1) asm volatile("hlt");
}

inline uint32_t sys_write(uint32_t fd, const void* buf, uint32_t count) {
    return syscall(Syscall::kWrite, fd, reinterpret_cast<uint32_t>(buf), count);
}

inline uint32_t sys_getpid() {
    return syscall(Syscall::kGetPid);
}

inline uint32_t sys_gettid() {
    return syscall(Syscall::kGetTid);
}

inline uint32_t sys_proc_count() {
    return syscall(Syscall::kProcCount);
}

inline uint32_t sys_getuid() {
    return syscall(Syscall::kGetUid);
}

inline uint32_t sys_getgid() {
    return syscall(Syscall::kGetGid);
}

inline uint32_t sys_open(const char* path, uint32_t flags = 0) {
    return syscall(Syscall::kOpen, reinterpret_cast<uint32_t>(path), flags);
}

inline uint32_t sys_read(uint32_t fd, void* buf, uint32_t count) {
    return syscall(Syscall::kRead, fd, reinterpret_cast<uint32_t>(buf), count);
}

inline uint32_t sys_close(uint32_t fd) {
    return syscall(Syscall::kClose, fd);
}

inline uint32_t sys_md(const char* path) {
    return syscall(Syscall::kmd, reinterpret_cast<uint32_t>(path));
}

struct DirEntry {
    char name[64];
    uint32_t is_directory;
};

inline uint32_t sys_readdir(const char* path, DirEntry* entries, uint32_t max_entries) {
    return syscall(
        Syscall::kReaddir,
        reinterpret_cast<uint32_t>(path),
        reinterpret_cast<uint32_t>(entries),
        max_entries);
}

inline uint32_t sys_rd(const char* path) {
    return syscall(Syscall::krd, reinterpret_cast<uint32_t>(path));
}

inline uint32_t sys_unlink(const char* path) {
    return syscall(Syscall::kUnlink, reinterpret_cast<uint32_t>(path));
}

inline uint32_t sys_sleep(uint32_t ticks) {
    return syscall(Syscall::kSleep, ticks);
}

inline uint32_t strlen(const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

inline uint32_t write_cstr(uint32_t fd, const char* s) {
    return sys_write(fd, s, strlen(s));
}

}  // namespace userland
