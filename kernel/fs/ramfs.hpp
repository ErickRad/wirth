#pragma once

#include "vfs.hpp"
#include <stdint.h>

#include "../sync/spinlock.hpp"

namespace kernel::fs {

class RamFs final : public FileSystem {
private:
    static constexpr uint32_t kMaxInodes = 64;
    static constexpr uint32_t kMaxFds = 32;

    Inode m_inodes[kMaxInodes];
    FileDescriptor m_fds[kMaxFds];
    kernel::sync::SpinLock m_lock;

    static bool normalize_path(const char* path, char* out_path, uint32_t out_size);
    Inode* find_inode(const char* path);
    Inode* create_inode(const char* path, bool is_directory);
    bool parent_directory_exists(const char* normalized_path);
    int alloc_fd();
    FileDescriptor* get_fd(int fd);
    bool ensure_capacity(Inode* inode, uint32_t required);

public:
    RamFs();
    void reset();
    bool init() override;
    int open(const char* path, uint32_t flags) override;
    int read(int fd, void* buf, uint32_t count) override;
    int write(int fd, const void* buf, uint32_t count) override;
    int close(int fd) override;
    int md(const char* path) override;
    int readdir(const char* path, DirEntry* entries, uint32_t max_entries) override;
    int rd(const char* path) override;
    int unlink(const char* path) override;
};

}  // namespace kernel::fs
