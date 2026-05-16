#pragma once

#include <stdint.h>

namespace kernel::fs {

class FileSystem;

enum OpenFlags : uint32_t {
    kOpenRead = 1u << 0,
    kOpenWrite = 1u << 1,
    kOpenCreate = 1u << 2,
    kOpenTruncate = 1u << 3,
};

struct Inode {
    bool used;
    bool is_directory;
    char name[64];
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
};

struct FileDescriptor {
    bool used;
    Inode* inode;
    uint32_t offset;
    uint32_t flags;
};

struct DirEntry {
    char name[64];
    uint32_t is_directory;
};

class FileSystem {
public:
    virtual ~FileSystem() {}

    virtual bool init() = 0;
    virtual int open(const char* path, uint32_t flags) = 0;
    virtual int read(int fd, void* buf, uint32_t count) = 0;
    virtual int write(int fd, const void* buf, uint32_t count) = 0;
    virtual int close(int fd) = 0;
    virtual int mkdir(const char* path) = 0;
    virtual int readdir(const char* path, DirEntry* entries, uint32_t max_entries) = 0;
    virtual int rmdir(const char* path) = 0;
    virtual int unlink(const char* path) = 0;
};

extern FileSystem* g_fs;

}  // namespace kernel::fs
