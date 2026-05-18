#include "syscall.hpp"

#include "../arch/x86/interrupts.hpp"
#include "../fs/vfs.hpp"
#include "../serial.hpp"
#include "../task/scheduler.hpp"
#include "../user_safety.hpp"

namespace {

constexpr uint32_t kErrorInvalidSyscall = 0xFFFFFFFF;
constexpr uint32_t kWriteChunkLimit = 4096;
constexpr uint32_t kMaxPathLen = 512;
constexpr uint32_t kMaxDirEntries = 32;

uint32_t sys_write(uint32_t fd, uint32_t buffer_ptr, uint32_t length) {
    const uint32_t safe_len = (length > kWriteChunkLimit) ? kWriteChunkLimit : length;

    if (safe_len == 0) {
        return 0;
    }
    
    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_buffer(buffer_ptr, safe_len)) {
        return kErrorInvalidSyscall;
    }
    
    const char* data = reinterpret_cast<const char*>(buffer_ptr);
    
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < safe_len; ++i) {
            kernel::serial::write_char(data[i]);
        }
    
        return safe_len;
    }
    
    if (kernel::fs::g_fs == nullptr) {
        return kErrorInvalidSyscall;
    }
    
    const int n = kernel::fs::g_fs->write(static_cast<int>(fd), data, safe_len);
    return (n < 0) ? kErrorInvalidSyscall : static_cast<uint32_t>(n);
}

[[noreturn]] void sys_exit(uint32_t code) {
    kernel::serial::write("[wirth] sys_exit code=0x");
    kernel::serial::write_hex(code);
    kernel::serial::write("\n");
    kernel::task::scheduler::exit_current();
    
    while (true) {
        asm volatile("sti; hlt");
    }
}

uint32_t sys_sleep(uint32_t ticks) {
    if (ticks == 0) {
        return 0;
    }
    
    kernel::task::scheduler::sleep_current(ticks, kernel::arch::x86::interrupts::ticks());
    
    while (kernel::task::scheduler::is_current_sleeping(kernel::arch::x86::interrupts::ticks())) {
        asm volatile("hlt");
    }
    
    return ticks;
}

uint32_t sys_open(uint32_t path_ptr, uint32_t flags) {
    if (kernel::fs::g_fs == nullptr || path_ptr == 0) {
        return kErrorInvalidSyscall;
    }
    
    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_string(path_ptr, kMaxPathLen)) {
        return kErrorInvalidSyscall;
    }
    
    const char* path = reinterpret_cast<const char*>(path_ptr);
    const int fd = kernel::fs::g_fs->open(path, flags);
    
    return (fd < 0) ? kErrorInvalidSyscall : static_cast<uint32_t>(fd);
}

uint32_t sys_read(uint32_t fd, uint32_t buffer_ptr, uint32_t count) {
    if (kernel::fs::g_fs == nullptr || buffer_ptr == 0) {
        return kErrorInvalidSyscall;
    }
    
    if (count == 0) {
        return 0;
    }
    
    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_buffer(buffer_ptr, count)) {
        return kErrorInvalidSyscall;
    }
    
    const int n =
        kernel::fs::g_fs->read(static_cast<int>(fd), reinterpret_cast<void*>(buffer_ptr), count);
    
    return (n < 0) ? kErrorInvalidSyscall : static_cast<uint32_t>(n);
}

uint32_t sys_close(uint32_t fd) {
    if (kernel::fs::g_fs == nullptr) {
        return kErrorInvalidSyscall;
    }

    const int result = kernel::fs::g_fs->close(static_cast<int>(fd));

    return (result < 0) ? kErrorInvalidSyscall : 0;
}

uint32_t sys_getpid() {
    return kernel::task::scheduler::current_process_id();
}

uint32_t sys_gettid() {
    return kernel::task::scheduler::current_thread_id();
}

uint32_t sys_proc_count() {
    return kernel::task::scheduler::process_count();
}

uint32_t sys_getuid() {
    return kernel::task::scheduler::current_user_id();
}

uint32_t sys_getgid() {
    return kernel::task::scheduler::current_group_id();
}

uint32_t sys_mkdir(uint32_t path_ptr) {
    if (kernel::fs::g_fs == nullptr || path_ptr == 0) {
        return kErrorInvalidSyscall;
    }

    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_string(path_ptr, kMaxPathLen)) {
        return kErrorInvalidSyscall;
    }

    const char* path = reinterpret_cast<const char*>(path_ptr);
    const int result = kernel::fs::g_fs->mkdir(path);

    return (result < 0) ? kErrorInvalidSyscall : 0;
}

uint32_t sys_readdir(uint32_t path_ptr, uint32_t entries_ptr, uint32_t max_entries) {
    if (kernel::fs::g_fs == nullptr || path_ptr == 0 || entries_ptr == 0) {
        return kErrorInvalidSyscall;
    }

    const uint32_t safe_max = (max_entries > kMaxDirEntries) ? kMaxDirEntries : max_entries;

    if (safe_max == 0) {
        return 0;
    }

    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_string(path_ptr, kMaxPathLen)) {
        return kErrorInvalidSyscall;
    }

    const uint32_t entries_bytes = safe_max * static_cast<uint32_t>(sizeof(kernel::fs::DirEntry));

    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_buffer(entries_ptr, entries_bytes)) {
        return kErrorInvalidSyscall;
    }

    const char* path = reinterpret_cast<const char*>(path_ptr);

    kernel::fs::DirEntry* entries = reinterpret_cast<kernel::fs::DirEntry*>(entries_ptr);

    const int n = kernel::fs::g_fs->readdir(path, entries, safe_max);

    return (n < 0) ? kErrorInvalidSyscall : static_cast<uint32_t>(n);
}

uint32_t sys_rmdir(uint32_t path_ptr) {
    if (kernel::fs::g_fs == nullptr || path_ptr == 0) {
        return kErrorInvalidSyscall;
    }

    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_string(path_ptr, kMaxPathLen)) {
        return kErrorInvalidSyscall;
    }

    const char* path = reinterpret_cast<const char*>(path_ptr);
    const int result = kernel::fs::g_fs->rmdir(path);

    return (result < 0) ? kErrorInvalidSyscall : 0;
}

uint32_t sys_unlink(uint32_t path_ptr) {
    if (kernel::fs::g_fs == nullptr || path_ptr == 0) {
        return kErrorInvalidSyscall;
    }

    if (kernel::task::scheduler::current_ring_level() == 3 &&
        !kernel::user_safety::is_user_string(path_ptr, kMaxPathLen)) {
        return kErrorInvalidSyscall;
    }

    const char* path = reinterpret_cast<const char*>(path_ptr);
    const int result = kernel::fs::g_fs->unlink(path);

    return (result < 0) ? kErrorInvalidSyscall : 0;
}

}  // namespace

extern "C" uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    switch (number) {
        case kernel::syscall::kWrite:
            return sys_write(arg0, arg1, arg2);

        case kernel::syscall::kExit:
            sys_exit(arg0);

        case kernel::syscall::kSleep:
            return sys_sleep(arg0);

        case kernel::syscall::kOpen:
            return sys_open(arg0, arg1);

        case kernel::syscall::kRead:
            return sys_read(arg0, arg1, arg2);

        case kernel::syscall::kClose:
            return sys_close(arg0);

        case kernel::syscall::kGetPid:
            return sys_getpid();

        case kernel::syscall::kGetTid:
            return sys_gettid();

        case kernel::syscall::kProcCount:
            return sys_proc_count();

        case kernel::syscall::kMkdir:
            return sys_mkdir(arg0);

        case kernel::syscall::kGetUid:
            return sys_getuid();

        case kernel::syscall::kGetGid:
            return sys_getgid();

        case kernel::syscall::kReaddir:
            return sys_readdir(arg0, arg1, arg2);

        case kernel::syscall::kRmdir:
            return sys_rmdir(arg0);

        case kernel::syscall::kUnlink:
            return sys_unlink(arg0);
            
        default:
            return kErrorInvalidSyscall;
    }
}
