#include "ramfs.hpp"

#include "../serial.hpp"

namespace {

constexpr int kFdOffset = 3;
constexpr uint8_t kInitElf[] = {
    0x7F, 0x45, 0x4C, 0x46, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x54, 0x00, 0x00, 0x08, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x62, 0x00, 0x00, 0x00,
    0x62, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0xB8, 0x02, 0x00, 0x00, 0x00, 0xBB, 0x2A, 0x00, 0x00, 0x00, 0xCD, 0x80,
    0xEB, 0xFE
};

void copy_text(char* dst, const char* src, uint32_t max_size) {
    uint32_t i = 0;

    while (i + 1 < max_size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

bool text_equal(const char* a, const char* b) {
    uint32_t i = 0;

    while (a[i] != '\0' && b[i] != '\0') {

        if (a[i] != b[i]) {
            return false;
        }

        ++i;
    }

    return a[i] == b[i];
}

uint32_t text_len(const char* text) {
    uint32_t n = 0;

    while (text[n] != '\0') {
        ++n;
    }

    return n;
}

bool text_starts_with(const char* text, const char* prefix) {
    uint32_t i = 0;

    while (prefix[i] != '\0') {

        if (text[i] != prefix[i]) {
            return false;
        }

        ++i;
    }

    return true;
}

void copy_bytes(void* dst, const void* src, uint32_t len) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    for (uint32_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
}

}  // namespace

namespace kernel::fs {

FileSystem* g_fs = nullptr;

RamFs::RamFs() : m_inodes{}, m_fds{} {}

void RamFs::reset() {
    kernel::sync::LockGuard guard(m_lock);

    for (uint32_t i = 0; i < kMaxInodes; ++i) {
        m_inodes[i].used = false;
        m_inodes[i].is_directory = false;
        m_inodes[i].name[0] = '\0';
        m_inodes[i].data = nullptr;
        m_inodes[i].size = 0;
        m_inodes[i].capacity = 0;
    }

    for (uint32_t i = 0; i < kMaxFds; ++i) {
        m_fds[i].used = false;
        m_fds[i].inode = nullptr;
        m_fds[i].offset = 0;
        m_fds[i].flags = 0;
    }

    create_inode("/", true);
}

bool RamFs::normalize_path(const char* path, char* out_path, uint32_t out_size) {
    if (path == nullptr || out_path == nullptr || out_size < 2) {
        return false;
    }
    if (path[0] != '/') {
        return false;
    }

    uint32_t out_i = 0;
    out_path[out_i++] = '/';

    uint32_t in_i = 1;
    bool prev_slash = true;

    while (path[in_i] != '\0') {
        const char c = path[in_i++];

        if (c == '/') {
            if (prev_slash) {
                continue;
            }

            if (out_i + 1 >= out_size) {
                return false;
            }

            out_path[out_i++] = '/';
            prev_slash = true;

            continue;
        }

        if (out_i + 1 >= out_size) {
            return false;
        }

        out_path[out_i++] = c;
        prev_slash = false;
    }

    if (out_i > 1 && out_path[out_i - 1] == '/') {
        --out_i;
    }

    out_path[out_i] = '\0';

    return true;
}

Inode* RamFs::find_inode(const char* path) {

    for (uint32_t i = 0; i < kMaxInodes; ++i) {
        if (!m_inodes[i].used) {
            continue;
        }

        if (text_equal(m_inodes[i].name, path)) {
            return &m_inodes[i];
        }
    }

    return nullptr;
}

Inode* RamFs::create_inode(const char* path, bool is_directory) {
    for (uint32_t i = 0; i < kMaxInodes; ++i) {

        if (m_inodes[i].used) {
            continue;
        }

        m_inodes[i].used = true;
        m_inodes[i].is_directory = is_directory;
        m_inodes[i].data = nullptr;

        m_inodes[i].size = 0;
        m_inodes[i].capacity = 0;

        copy_text(m_inodes[i].name, path, sizeof(m_inodes[i].name));

        return &m_inodes[i];
    }

    return nullptr;
}

bool RamFs::parent_directory_exists(const char* normalized_path) {
    if (text_equal(normalized_path, "/")) {
        return true;
    }

    const uint32_t len = text_len(normalized_path);

    if (len < 2) {
        return false;
    }

    char parent[64];

    if (len >= sizeof(parent)) {
        return false;
    }

    copy_text(parent, normalized_path, sizeof(parent));
    int last_slash = -1;

    for (uint32_t i = 0; i < len; ++i) {

        if (parent[i] == '/') {
            last_slash = static_cast<int>(i);
        }
    }

    if (last_slash <= 0) {
        parent[1] = '\0';

    } else {
        parent[last_slash] = '\0';
    }

    Inode* dir = find_inode(parent);
    return dir != nullptr && dir->is_directory;
}

int RamFs::alloc_fd() {

    for (uint32_t i = 0; i < kMaxFds; ++i) {

        if (!m_fds[i].used) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

FileDescriptor* RamFs::get_fd(int fd) {
    const int index = fd - kFdOffset;

    if (index < 0 || static_cast<uint32_t>(index) >= kMaxFds) {
        return nullptr;
    }

    if (!m_fds[index].used) {
        return nullptr;
    }

    return &m_fds[index];
}

bool RamFs::ensure_capacity(Inode* inode, uint32_t required) {
    if (required <= inode->capacity) {
        return true;
    }

    uint32_t new_capacity = (inode->capacity == 0) ? 64 : inode->capacity;

    while (new_capacity < required) {
        new_capacity *= 2;
    }

    uint8_t* new_data = new uint8_t[new_capacity];

    if (new_data == nullptr) {
        return false;
    }

    if (inode->data != nullptr && inode->size > 0) {
        copy_bytes(new_data, inode->data, inode->size);
    }

    inode->data = new_data;
    inode->capacity = new_capacity;
    return true;
}

bool RamFs::init() {
    kernel::sync::LockGuard guard(m_lock);

    for (uint32_t i = 0; i < kMaxInodes; ++i) {
        m_inodes[i].used = false;
        m_inodes[i].is_directory = false;
        m_inodes[i].name[0] = '\0';
        m_inodes[i].data = nullptr;
        m_inodes[i].size = 0;
        m_inodes[i].capacity = 0;
    }

    for (uint32_t i = 0; i < kMaxFds; ++i) {
        m_fds[i].used = false;
        m_fds[i].inode = nullptr;
        m_fds[i].offset = 0;
        m_fds[i].flags = 0;
    }

    Inode* root = create_inode("/", true);

    if (root == nullptr) {
        return false;
    }

    Inode* test = create_inode("/test.txt", false);

    if (test == nullptr) {
        return false;
    }

    constexpr const char kDefaultText[] = "Hello from ramfs!\n";
    constexpr uint32_t kDefaultLen = static_cast<uint32_t>(sizeof(kDefaultText) - 1);

    if (!ensure_capacity(test, kDefaultLen)) {
        return false;
    }

    copy_bytes(test->data, kDefaultText, kDefaultLen);
    test->size = kDefaultLen;

    Inode* bin = create_inode("/bin", true);
    if (bin == nullptr) {
        return false;
    }

    Inode* home = create_inode("/home", true);
    if (home == nullptr) {
        return false;
    }

    Inode* home_root = create_inode("/home/root", true);
    if (home_root == nullptr) {
        return false;
    }

    Inode* root_home = create_inode("/root", true);
    if (root_home == nullptr) {
        return false;
    }

    Inode* etc = create_inode("/etc", true);
    if (etc == nullptr) {
        return false;
    }

    Inode* usr = create_inode("/usr", true);
    if (usr == nullptr) {
        return false;
    }

    Inode* usr_bin = create_inode("/usr/bin", true);
    if (usr_bin == nullptr) {
        return false;
    }

    Inode* usr_share = create_inode("/usr/share", true);
    if (usr_share == nullptr) {
        return false;
    }

    Inode* usr_share_doc = create_inode("/usr/share/doc", true);
    if (usr_share_doc == nullptr) {
        return false;
    }

    Inode* usr_share_doc_wirth = create_inode("/usr/share/doc/wirth", true);
    if (usr_share_doc_wirth == nullptr) {
        return false;
    }

    Inode* var = create_inode("/var", true);
    if (var == nullptr) {
        return false;
    }

    Inode* var_lib = create_inode("/var/lib", true);
    if (var_lib == nullptr) {
        return false;
    }

    Inode* var_lib_wirth = create_inode("/var/lib/wirth", true);
    if (var_lib_wirth == nullptr) {
        return false;
    }

    Inode* var_lib_pkgdb = create_inode("/var/lib/wirth/pkgdb", true);
    if (var_lib_pkgdb == nullptr) {
        return false;
    }

    Inode* opt = create_inode("/opt", true);
    if (opt == nullptr) {
        return false;
    }

    Inode* opt_demo = create_inode("/opt/demo", true);
    if (opt_demo == nullptr) {
        return false;
    }

    Inode* etc_apt = create_inode("/etc/apt", true);
    if (etc_apt == nullptr) {
        return false;
    }

    Inode* etc_apt_sources = create_inode("/etc/apt/sources.list.d", true);
    if (etc_apt_sources == nullptr) {
        return false;
    }

    Inode* passwd = create_inode("/etc/passwd", false);
    if (passwd == nullptr) {
        return false;
    }

    constexpr const char kPasswd[] = "root:x:0:0:root:/root:/bin/init.elf\n";
    constexpr uint32_t kPasswdLen = static_cast<uint32_t>(sizeof(kPasswd) - 1);
    
    if (!ensure_capacity(passwd, kPasswdLen)) {
        return false;
    }
    copy_bytes(passwd->data, kPasswd, kPasswdLen);
    passwd->size = kPasswdLen;

    Inode* root_profile = create_inode("/root/.profile", false);
    if (root_profile == nullptr) {
        return false;
    }
    
    constexpr const char kRootProfile[] = "";
    constexpr uint32_t kRootProfileLen = static_cast<uint32_t>(sizeof(kRootProfile) - 1);
    
    if (!ensure_capacity(root_profile, kRootProfileLen)) {
        return false;
    }
    
    copy_bytes(root_profile->data, kRootProfile, kRootProfileLen);
    root_profile->size = kRootProfileLen;

    constexpr const char* kHomeEntries[] = {
        "Desktop",
        "Documents",
        "Downloads",
        "Images",
        "Musics",
        "Templates",
        "Videos",
        "Workspace",
        "Docs",
    };

    auto seed_home = [&](const char* base) -> bool {
        for (uint32_t i = 0; i < static_cast<uint32_t>(sizeof(kHomeEntries) / sizeof(kHomeEntries[0])); ++i) {
            char path[64];
            uint32_t pos = 0;
            
            while (base[pos] != '\0' && pos + 1 < sizeof(path)) {
                path[pos] = base[pos];
                ++pos;
            }
            
            if (pos == 0 || pos + 1 >= sizeof(path)) {
                return false;
            }
            
            if (path[pos - 1] != '/') {
                
                if (pos + 1 >= sizeof(path)) {
                    return false;
                }
                path[pos++] = '/';
            }
            
            uint32_t j = 0;
            
            while (kHomeEntries[i][j] != '\0' && pos + 1 < sizeof(path)) {
                path[pos++] = kHomeEntries[i][j++];
            
            }
            
            if (kHomeEntries[i][j] != '\0') {
                return false;
            }
            
            path[pos] = '\0';
            
            if (create_inode(path, true) == nullptr) {
                return false;
            }
        }
        
        return true;
    };
    
    if (!seed_home("/root")) {
        return false;
    }
    
    if (!seed_home("/home/root")) {
        return false;
    }

    Inode* init_elf = create_inode("/bin/init.elf", false);
    if (init_elf == nullptr) {
        return false;
    }
    
    if (!ensure_capacity(init_elf, sizeof(kInitElf))) {
        return false;
    }
    
    copy_bytes(init_elf->data, kInitElf, sizeof(kInitElf));
    init_elf->size = sizeof(kInitElf);

    // ramfs mounted
    return true;
}

int RamFs::open(const char* path, uint32_t flags) {
    kernel::sync::LockGuard guard(m_lock);

    char normalized[64];
    if (!normalize_path(path, normalized, sizeof(normalized)) || text_equal(normalized, "/")) {
        return -1;
    }
    
    Inode* inode = find_inode(normalized);
    if (inode == nullptr) {
        if ((flags & kOpenCreate) == 0) {
            return -1;
        }
        
        if (!parent_directory_exists(normalized)) {
            return -1;
        }
        
        inode = create_inode(normalized, false);
        
        if (inode == nullptr) {
            return -1;
        }
    }
    
    if (inode->is_directory) {
        return -1;
    }

    if ((flags & kOpenTruncate) != 0 && (flags & kOpenWrite) != 0) {
        inode->size = 0;
    }

    const int fd_index = alloc_fd();
    
    if (fd_index < 0) {
        return -1;
    }

    FileDescriptor& fd = m_fds[fd_index];
    fd.used = true;
    fd.inode = inode;
    fd.offset = 0;
    fd.flags = flags;
    
    return fd_index + kFdOffset;
}

int RamFs::read(int fd, void* buf, uint32_t count) {
    kernel::sync::LockGuard guard(m_lock);

    if (buf == nullptr) {
        return -1;
    }
    
    FileDescriptor* desc = get_fd(fd);
    
    if (desc == nullptr || desc->inode == nullptr) {
        return -1;
    }
    
    if ((desc->flags & kOpenRead) == 0) {
        return -1;
    }
    
    Inode* inode = desc->inode;
    if (desc->offset >= inode->size) {
        return 0;
    }
    
    const uint32_t remaining = inode->size - desc->offset;
    const uint32_t n = (count < remaining) ? count : remaining;
    
    copy_bytes(buf, inode->data + desc->offset, n);
    desc->offset += n;
    
    return static_cast<int>(n);
}

int RamFs::write(int fd, const void* buf, uint32_t count) {
    
    kernel::sync::LockGuard guard(m_lock);

    if (buf == nullptr) {
        return -1;
    }
    
    FileDescriptor* desc = get_fd(fd);
    if (desc == nullptr || desc->inode == nullptr) {
        return -1;
    }
    
    if ((desc->flags & kOpenWrite) == 0) {
        return -1;
    }
    
    Inode* inode = desc->inode;
    const uint32_t end = desc->offset + count;
    
    if (!ensure_capacity(inode, end)) {
        return -1;
    }
    
    copy_bytes(inode->data + desc->offset, buf, count);
    desc->offset = end;
    
    if (end > inode->size) {
        inode->size = end;
    }
    
    return static_cast<int>(count);
}

int RamFs::close(int fd) {
    kernel::sync::LockGuard guard(m_lock);

    FileDescriptor* desc = get_fd(fd);
    
    if (desc == nullptr) {
        return -1;
    }
    
    desc->used = false;
    desc->inode = nullptr;
    desc->offset = 0;
    desc->flags = 0;
    
    return 0;
}

int RamFs::mkdir(const char* path) {
    kernel::sync::LockGuard guard(m_lock);


    char normalized[64];
    
    if (!normalize_path(path, normalized, sizeof(normalized))) {
        return -1;
    }
    
    if (text_equal(normalized, "/")) {
        return 0;
    }

    Inode* existing = find_inode(normalized);
    
    if (existing != nullptr) {
        return existing->is_directory ? 0 : -1;
    }
    
    if (!parent_directory_exists(normalized)) {
        return -1;
    }
    
    return (create_inode(normalized, true) == nullptr) ? -1 : 0;
}

int RamFs::readdir(const char* path, DirEntry* entries, uint32_t max_entries) {
    kernel::sync::LockGuard guard(m_lock);

    if (entries == nullptr || max_entries == 0) {
        return -1;
    }
    char normalized[64];
    
    if (!normalize_path(path, normalized, sizeof(normalized))) {
        return -1;
    }

    Inode* dir = find_inode(normalized);
    if (dir == nullptr || !dir->is_directory) {
        return -1;
    }

    uint32_t out_count = 0;
    const uint32_t dir_len = text_len(normalized);
    
    for (uint32_t i = 0; i < kMaxInodes && out_count < max_entries; ++i) {
        const Inode& inode = m_inodes[i];
        if (!inode.used || text_equal(inode.name, normalized)) {
            continue;
        }

        const char* child_name = nullptr;
        
        if (text_equal(normalized, "/")) {
            if (inode.name[0] != '/' || inode.name[1] == '\0') {
                continue;
            }
            child_name = inode.name + 1;
        
        } else {
            char prefix[64];
            
            copy_text(prefix, normalized, sizeof(prefix));
            const uint32_t prefix_len = text_len(prefix);
            
            if (prefix_len + 1 >= sizeof(prefix)) {
                continue;
            }
            
            prefix[prefix_len] = '/';
            prefix[prefix_len + 1] = '\0';
            
            if (!text_starts_with(inode.name, prefix)) {
                continue;
            }
            
            child_name = inode.name + dir_len + 1;
        }

        if (child_name == nullptr || child_name[0] == '\0') {
            continue;
        }
        
        bool direct_child = true;
        
        for (uint32_t j = 0; child_name[j] != '\0'; ++j) {
            if (child_name[j] == '/') {
                direct_child = false;
                break;
            }
        }
        
        if (!direct_child) {
            continue;
        }

        copy_text(entries[out_count].name, child_name, sizeof(entries[out_count].name));
        entries[out_count].is_directory = inode.is_directory;
        ++out_count;
    }

    return static_cast<int>(out_count);
}

int RamFs::rmdir(const char* path) {
    kernel::sync::LockGuard guard(m_lock);

    char normalized[64];
    
    if (!normalize_path(path, normalized, sizeof(normalized)) || text_equal(normalized, "/")) {
        return -1;
    }

    Inode* inode = find_inode(normalized);
    if (inode == nullptr || !inode->is_directory) {
        return -1;
    }

    char prefix[64];
    copy_text(prefix, normalized, sizeof(prefix));
    const uint32_t prefix_len = text_len(prefix);
    
    if (prefix_len + 1 >= sizeof(prefix)) {
        return -1;
    }
    
    prefix[prefix_len] = '/';
    prefix[prefix_len + 1] = '\0';

    for (uint32_t i = 0; i < kMaxInodes; ++i) {
        if (!m_inodes[i].used || text_equal(m_inodes[i].name, normalized)) {
            continue;
        }
    
        if (text_starts_with(m_inodes[i].name, prefix)) {
            return -1;
        }
    }

    for (uint32_t i = 0; i < kMaxFds; ++i) {
        if (m_fds[i].used && m_fds[i].inode == inode) {
            return -1;
        }
    }

    inode->used = false;
    inode->is_directory = false;
    inode->name[0] = '\0';
    inode->size = 0;
    inode->capacity = 0;
    inode->data = nullptr;
    
    return 0;
}

int RamFs::unlink(const char* path) {
    kernel::sync::LockGuard guard(m_lock);

    char normalized[64];
    
    if (!normalize_path(path, normalized, sizeof(normalized)) || text_equal(normalized, "/")) {
        return -1;
    }

    Inode* inode = find_inode(normalized);
    if (inode == nullptr || inode->is_directory) {
        return -1;
    }

    for (uint32_t i = 0; i < kMaxFds; ++i) {
        if (m_fds[i].used && m_fds[i].inode == inode) {
            return -1;
        }
    }

    inode->used = false;
    inode->name[0] = '\0';
    inode->size = 0;
    inode->capacity = 0;
    inode->data = nullptr;
    
    return 0;
}

}  // namespace kernel::fs
