extern "C" const char kEmbeddedRootfsSeed[];
extern "C" const unsigned kEmbeddedRootfsSeedSize;
#include <stdint.h>
#include <stddef.h>

#include "fs/ramfs.hpp"
#include "fs/vfs.hpp"
#include "arch/x86/io.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "boot/multiboot2.hpp"
#include "serial.hpp"

namespace {

constexpr uint32_t kMaxLine = 128;
constexpr uint32_t kMaxArgs = 8;
constexpr uint32_t kMaxCommands = 64;
constexpr size_t kKernelHeapSize = 1024u * 1024u;

alignas(16) static uint8_t g_kernel_heap[kKernelHeapSize] = {};
static size_t g_kernel_heap_used = 0;

static kernel::fs::RamFs g_ramfs;

struct CommandAlias {
    const char* name;
    const char* base;
    uint32_t variant_limit;
};

static const CommandAlias kCommandAliases[kMaxCommands] = {
    {"clear", "clear", 49}, {"cls", "clear", 49}, {"reset", "clear", 49}, {"clean", "clear", 49},
    {"wipe", "clear", 49}, {"screen", "clear", 49},
    {"ld", "ld", 199}, {"ls", "ld", 199}, {"dir", "ld", 199}, {"ll", "ld", 199},
    {"la", "ld", 199}, {"l", "ld", 199}, {"list", "ld", 199}, {"listdir", "ld", 199},
    {"contents", "ld", 199}, {"browse", "ld", 199}, {"showdir", "ld", 199},
    {"showfiles", "ld", 199}, {"list-directory", "ld", 199},
    {"md", "md", 199}, {"mkdir", "md", 199}, {"mk", "md", 199}, {"makedir", "md", 199},
    {"newdir", "md", 199}, {"createdir", "md", 199}, {"create-dir", "md", 199},
    {"cd", "cd", 99}, {"chdir", "cd", 99}, {"changedir", "cd", 99}, {"goto", "cd", 99},
    {"pwd", "pwd", 49}, {"cwd", "pwd", 49}, {"whereami", "pwd", 49}, {"here", "pwd", 49},
    {"help", "help", 49}, {"?", "help", 49}, {"h", "help", 49}, {"commands", "help", 49},
    {"cmds", "help", 49}, {"apropos", "help", 49}, {"usage", "help", 49},
};

void copy_text(char* dst, const char* src, uint32_t max_size) {
    if (dst == nullptr || src == nullptr || max_size == 0) {
        return;
    }

    uint32_t i = 0;

    while (i + 1 < max_size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

bool text_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }

    uint32_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {

        if (a[i] != b[i]) {
            return false;
        }
        ++i;
    }
    return a[i] == b[i];
}

bool text_starts_with(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    uint32_t i = 0;

    while (prefix[i] != '\0') {

        if (text[i] != prefix[i]) {
            return false;
        }

        ++i;
    }

    return true;
}

uint32_t text_len(const char* text) {
    uint32_t len = 0;

    while (text != nullptr && text[len] != '\0') {
        ++len;
    }

    return len;
}

bool path_equals(const char* a, const char* b) {
    return text_equal(a, b);
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool path_starts_with(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    uint32_t i = 0;
    while (prefix[i] != '\0') {

        if (text[i] != prefix[i]) {

            return false;
        }

        ++i;
    }

    return true;
}

void* kernel_alloc(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    const size_t mask = alignment - 1;
    const size_t aligned = (g_kernel_heap_used + mask) & ~mask;

    if (aligned + size > kKernelHeapSize) {
        return nullptr;
    }

    g_kernel_heap_used = aligned + size;

    return g_kernel_heap + aligned;
}

}  // namespace

void* operator new(size_t size) {
    return kernel_alloc(size, 16);
}

void* operator new[](size_t size) {
    return kernel_alloc(size, 16);
}

void operator delete(void* ptr) noexcept {
    (void)ptr;
}

void operator delete[](void* ptr) noexcept {
    (void)ptr;
}

void operator delete(void* ptr, size_t) noexcept {
    (void)ptr;
}

void operator delete[](void* ptr, size_t) noexcept {
    (void)ptr;
}

namespace {

bool resolve_special_path_prefix(const char* cwd, const char* input, char* out, uint32_t out_size) {
    if (input == nullptr || out == nullptr || out_size == 0) {
        return false;
    }

    if (input[0] == '~') {

        if (input[1] == '\0') {
            copy_text(out, "/home", out_size);
            return true;
        }

        if (input[1] == '/') {
            copy_text(out, "/home", out_size);
            const uint32_t len = text_len(out);

            if (len + 1 >= out_size) {
                return false;
            }

            out[len] = '/';
            out[len + 1] = '\0';

            copy_text(out + len + 1, input + 2, out_size - len - 1);

            return true;
        }
    }

    if (input[0] == '/') {
        copy_text(out, input, out_size);
        return true;
    }

    if (cwd == nullptr) {
        return false;
    }

    if (path_equals(cwd, "/")) {
        copy_text(out, "/", out_size);
        const uint32_t len = text_len(out);

        if (input[0] != '\0' && len + 1 < out_size) {
            out[len] = '/';
            out[len + 1] = '\0';

            copy_text(out + len + 1, input, out_size - len - 1);
        }

        return true;
    }

    copy_text(out, cwd, out_size);
    const uint32_t len = text_len(out);
    
    if (len + 1 >= out_size) {
        return false;
    }

    out[len] = '/';
    out[len + 1] = '\0';
    copy_text(out + len + 1, input, out_size - len - 1);
    return true;
}

bool normalize_path(char* path, uint32_t path_size) {
    if (path == nullptr || path_size == 0 || path[0] == '\0') {
        return false;
    }

    char parts[8][32] = {};
    uint32_t part_count = 0;
    char token[32] = {};
    uint32_t token_len = 0;
    const bool absolute = path[0] == '/';

    for (uint32_t i = 0; ; ++i) {
        const char c = path[i];
        if (c == '/' || c == '\0') {
            token[token_len] = '\0';
            if (token_len > 0) {
                if (text_equal(token, ".")) {
                } else if (text_equal(token, "..")) {
                    if (part_count > 0) {
                        --part_count;
                    }
                } else if (part_count < 8) {
                    copy_text(parts[part_count], token, sizeof(parts[part_count]));
                    ++part_count;
                }
            }
            token_len = 0;
            if (c == '\0') {
                break;
            }
        } else if (token_len + 1 < sizeof(token)) {
            token[token_len++] = c;
        }
    }

    uint32_t out = 0;
    if (absolute) {
        path[out++] = '/';
    }

    for (uint32_t i = 0; i < part_count && out + 1 < path_size; ++i) {
        if (out > 0 && path[out - 1] != '/') {
            path[out++] = '/';
        }
        for (uint32_t j = 0; parts[i][j] != '\0' && out + 1 < path_size; ++j) {
            path[out++] = parts[i][j];
        }
    }

    if (out == 0) {
        path[out++] = absolute ? '/' : '.';
    }

    path[out] = '\0';
    return true;
}

bool resolve_path(const char* cwd, const char* input, char* out, uint32_t out_size) {
    if (!resolve_special_path_prefix(cwd, input, out, out_size)) {
        return false;
    }

    return normalize_path(out, out_size);
}

bool filesystem_ready() {
    return kernel::fs::g_fs != nullptr;
}

bool directory_exists(const char* path) {
    if (!filesystem_ready() || path == nullptr) {
        return false;
    }

    kernel::fs::DirEntry probe[1] = {};
    return kernel::fs::g_fs->readdir(path, probe, 1) >= 0;
}

void apply_rootfs_seed_line(char* line) {
    if (line == nullptr || kernel::fs::g_fs == nullptr) {
        return;
    }

    char* cursor = line;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    if (*cursor == '\0' || *cursor == '#') {
        return;
    }

    char* command = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        ++cursor;
    }

    if (*cursor != '\0') {
        *cursor++ = '\0';
    }

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    if (*cursor == '\0') {
        return;
    }

    if (text_equal(command, "mkdir") || text_equal(command, "dir") || text_equal(command, "md")) {
        kernel::fs::g_fs->mkdir(cursor);
    }
}

void load_rootfs_seed(const char* data, uint32_t size) {
    if (data == nullptr || size == 0 || kernel::fs::g_fs == nullptr) {
        return;
    }

    char line[96] = {};
    uint32_t line_len = 0;

    for (uint32_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line[line_len] = '\0';
                apply_rootfs_seed_line(line);
                line_len = 0;
            }
            continue;
        }

        if (line_len + 1 < sizeof(line)) {
            line[line_len++] = c;
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        apply_rootfs_seed_line(line);
    }
}

struct RootfsModuleContext {
    bool loaded;
};

void rootfs_module_visitor(const kernel::boot::multiboot2::ModuleView& module, void* user_data) {
    auto* ctx = reinterpret_cast<RootfsModuleContext*>(user_data);
    if (ctx == nullptr || ctx->loaded) {
        return;
    }

    const char* const module_data = reinterpret_cast<const char*>(module.start_addr);
    const uint32_t module_size = module.end_addr > module.start_addr ? module.end_addr - module.start_addr : 0;
    load_rootfs_seed(module_data, module_size);
    ctx->loaded = true;
}

bool get_parent_path(const char* path, char* parent, uint32_t parent_size) {
    if (path == nullptr || parent == nullptr || parent_size == 0) {
        return false;
    }

    if (path_equals(path, "/")) {
        copy_text(parent, "/", parent_size);
        return true;
    }

    const uint32_t len = text_len(path);
    if (len == 0 || len >= parent_size) {
        return false;
    }

    int last_slash = -1;
    for (uint32_t i = 0; i < len; ++i) {
        if (path[i] == '/') {
            last_slash = static_cast<int>(i);
        }
    }

    if (last_slash <= 0) {
        copy_text(parent, "/", parent_size);
        return true;
    }

    for (int i = 0; i < last_slash; ++i) {
        parent[i] = path[i];
    }
    parent[last_slash] = '\0';
    return true;
}

void normalize_special_path(char* path, uint32_t path_size) {
    if (path == nullptr || path_size == 0) {
        return;
    }

    // Kernel-root layout: map user-facing aliases to kernel's root tree.
    if (path_equals(path, "/root")) {
        copy_text(path, "/root", path_size);
    } else if (path_equals(path, "/user")) {
        copy_text(path, "/usr", path_size);
    } else if (path_equals(path, "/temp")) {
        copy_text(path, "/tmp", path_size);
    }
}

bool matches_numbered_alias(const char* command, const char* base, uint32_t max_variant) {
    if (text_equal(command, base)) {
        return true;
    }

    if (!text_starts_with(command, base)) {
        return false;
    }

    const uint32_t base_len = text_len(base);
    const char* suffix = command + base_len;

    if (*suffix == '\0') {
        return true;
    }

    uint32_t value = 0;
    uint32_t digits = 0;

    while (is_digit(*suffix)) {
        value = value * 10u + static_cast<uint32_t>(*suffix - '0');
        ++suffix;
        ++digits;

        if (value > max_variant) {
            return false;
        }
    }

    return digits > 0 && *suffix == '\0';
}

void format_prompt_path(const char* cwd, char* out, uint32_t out_size) {
    if (cwd == nullptr || out == nullptr || out_size == 0) {
        return;
    }

    if (text_equal(cwd, "/home")) {
        copy_text(out, "~", out_size);
        normalize_special_path(out, out_size);
        return;
    }

    copy_text(out, cwd, out_size);
}

const char* canonical_command_name(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }

    for (uint32_t i = 0; i < kMaxCommands; ++i) {
        const CommandAlias& alias = kCommandAliases[i];
        if (alias.name == nullptr) {
            continue;
        }

        if (text_equal(name, alias.name) || (path_starts_with(name, alias.base) && matches_numbered_alias(name, alias.base, alias.variant_limit))) {
            return alias.name;
        }
    }

    return nullptr;
}

void print_command_candidates(const char* prefix) {
    bool any = false;

    for (uint32_t i = 0; i < kMaxCommands; ++i) {
        const char* name = kCommandAliases[i].name;
        if (name == nullptr || !text_starts_with(name, prefix)) {
            continue;
        }

        kernel::serial::write(name);
        kernel::serial::write(" ");
        any = true;
    }

    if (any) {
        kernel::serial::write("\n");
    }
}

bool complete_command_token(char* line, uint32_t& pos) {
    uint32_t start = 0;
    while (start < pos && line[start] == ' ') {
        ++start;
    }

    uint32_t end = start;
    while (end < pos && line[end] != ' ' && line[end] != '\t') {
        ++end;
    }

    char prefix[32] = {};
    uint32_t prefix_len = 0;
    while (start + prefix_len < end && prefix_len + 1 < sizeof(prefix)) {
        prefix[prefix_len] = line[start + prefix_len];
        ++prefix_len;
    }
    prefix[prefix_len] = '\0';

    const char* first_match = nullptr;
    uint32_t common_len = 0;
    uint32_t match_count = 0;

    for (uint32_t i = 0; i < kMaxCommands; ++i) {
        const char* name = kCommandAliases[i].name;
        if (name == nullptr) {
            continue;
        }

        if (!text_starts_with(name, prefix)) {
            continue;
        }

        ++match_count;
        if (first_match == nullptr) {
            first_match = name;
            common_len = text_len(name);
            continue;
        }

        uint32_t shared = 0;
        while (first_match[shared] != '\0' && name[shared] != '\0' && first_match[shared] == name[shared]) {
            ++shared;
        }

        if (shared < common_len) {
            common_len = shared;
        }
    }

    if (match_count == 0) {
        return false;
    }

    if (match_count > 1) {
        if (common_len > prefix_len) {
            while (prefix_len < common_len && pos + 1 < kMaxLine) {
                line[pos++] = first_match[prefix_len++];
                kernel::serial::write_char(first_match[prefix_len - 1]);
            }
            return true;
        }

        kernel::serial::write("\n");
        print_command_candidates(prefix);
        return true;
    }

    const uint32_t name_len = text_len(first_match);
    while (prefix_len < name_len && pos + 1 < kMaxLine) {
        line[pos++] = first_match[prefix_len++];
        kernel::serial::write_char(first_match[prefix_len - 1]);
    }

    return true;
}

bool complete_path_token(char* line, uint32_t& pos, const char* cwd) {
    uint32_t start = pos;
    while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t') {
        --start;
    }

    if (start >= pos) {
        return false;
    }

    char token[32] = {};
    uint32_t token_len = 0;
    while (start + token_len < pos && token_len + 1 < sizeof(token)) {
        token[token_len] = line[start + token_len];
        ++token_len;
    }
    token[token_len] = '\0';

    char resolved[32] = {};
    if (!resolve_path(cwd, token, resolved, sizeof(resolved))) {
        return false;
    }

    char parent[32] = {};
    if (!get_parent_path(resolved, parent, sizeof(parent))) {
        return false;
    }

    const uint32_t prefix_len = text_len(token);
    const char* suggestion = nullptr;
    uint32_t match_count = 0;
    kernel::fs::DirEntry entries[32] = {};
    const int entry_count = filesystem_ready() ? kernel::fs::g_fs->readdir(parent, entries, 32) : -1;

    if (entry_count < 0) {
        return false;
    }

    for (int i = 0; i < entry_count; ++i) {
        if (!entries[i].is_directory) {
            continue;
        }

        if (prefix_len > 0 && !text_starts_with(entries[i].name, token)) {
            continue;
        }

        ++match_count;
        if (suggestion == nullptr) {
            suggestion = entries[i].name;
        }
    }

    if (match_count == 0 || suggestion == nullptr) {
        return false;
    }

    if (match_count > 1) {
        kernel::serial::write("\n");
        for (int i = 0; i < entry_count; ++i) {
            if (entries[i].is_directory && (prefix_len == 0 || text_starts_with(entries[i].name, token))) {
                kernel::serial::write(entries[i].name);
                kernel::serial::write(" ");
            }
        }
        kernel::serial::write("\n");
    }

    return true;
}

int split_args(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* cursor = line;

    while (*cursor != '\0' && argc < max_args) {
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }

        if (*cursor == '\0') {
            break;
        }

        argv[argc++] = cursor;

        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            ++cursor;
        }

        if (*cursor == '\0') {
            break;
        }

        *cursor++ = '\0';
    }

    return argc;
}

void print_prompt(const char* cwd) {
    char prompt_path[32] = {};
    format_prompt_path(cwd, prompt_path, sizeof(prompt_path));
    kernel::serial::write("root@wirth:");
    kernel::serial::write(prompt_path);
    kernel::serial::write("$ ");
}

void print_directory_listing(const char* path) {
    if (!filesystem_ready()) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    kernel::fs::DirEntry entries[32] = {};
    const int count = kernel::fs::g_fs->readdir(path, entries, 32);
    if (count < 0) {
        kernel::serial::write("ld: directory not found\n");
        return;
    }

    if (count == 0) {
        kernel::serial::write("\n");
        return;
    }

    for (int i = 0; i < count; ++i) {
        kernel::serial::write(entries[i].name);
        kernel::serial::write("\n");
    }
}

void execute_command(int argc, char* argv[], char* cwd) {
    if (argc <= 0 || argv == nullptr || cwd == nullptr) {
        return;
    }

    const char* command_name = canonical_command_name(argv[0]);

    if (command_name != nullptr && (text_equal(command_name, "clear") || text_equal(command_name, "cls") ||
        text_equal(command_name, "reset") || text_equal(command_name, "clean") || text_equal(command_name, "wipe") ||
        text_equal(command_name, "screen"))) {
        kernel::serial::write("\x1b[2J\x1b[H");
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "pwd") || text_equal(command_name, "cwd") ||
        text_equal(command_name, "whereami") || text_equal(command_name, "here"))) {
        kernel::serial::write(cwd);
        kernel::serial::write("\n");
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "cd") || text_equal(command_name, "chdir") ||
        text_equal(command_name, "changedir") || text_equal(command_name, "goto"))) {
        if (argc < 2) {
            copy_text(cwd, "/home", 32);
            return;
        }

        char resolved[32] = {};
        resolve_path(cwd, argv[1], resolved, sizeof(resolved));

        if (resolved[0] == '\0' || !directory_exists(resolved)) {
            kernel::serial::write("cd: directory not found\n");
            return;
        }

        copy_text(cwd, resolved, 32);
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "ld") || text_equal(command_name, "ls") ||
        text_equal(command_name, "dir") || text_equal(command_name, "ll") || text_equal(command_name, "la") ||
        text_equal(command_name, "l") || text_equal(command_name, "list") || text_equal(command_name, "listdir") ||
        text_equal(command_name, "contents") || text_equal(command_name, "browse") ||
        text_equal(command_name, "showdir") || text_equal(command_name, "showfiles") ||
        text_equal(command_name, "list-directory"))) {
        char path[32] = {};

        if (argc < 2) {
            copy_text(path, cwd, sizeof(path));
        } else {
            resolve_path(cwd, argv[1], path, sizeof(path));
        }

        if (!directory_exists(path)) {
            kernel::serial::write("ld: directory not found\n");
            return;
        }

        print_directory_listing(path);
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "md") || text_equal(command_name, "mkdir") ||
        text_equal(command_name, "mk") || text_equal(command_name, "makedir") || text_equal(command_name, "newdir") ||
        text_equal(command_name, "createdir") || text_equal(command_name, "create-dir"))) {
        if (argc < 2) {
            kernel::serial::write("md <dir>\n");
            return;
        }

        char path[32] = {};
        resolve_path(cwd, argv[1], path, sizeof(path));

        if (path[0] == '\0') {
            kernel::serial::write("md: invalid path\n");
            return;
        }

        if (filesystem_ready() && kernel::fs::g_fs->mkdir(path) == 0) {
            return;
        }

        if (directory_exists(path)) {
            kernel::serial::write("md: directory already exists\n");
            return;
        }

        kernel::serial::write("md: could not create directory\n");
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "help") || text_equal(command_name, "?") ||
        text_equal(command_name, "h") || text_equal(command_name, "commands") || text_equal(command_name, "cmds") ||
        text_equal(command_name, "apropos") || text_equal(command_name, "usage"))) {
        kernel::serial::write("commands: ld, md, cd, pwd, clear, plus numbered families like ld12/md7\n");
        return;
    }

    kernel::serial::write("unknown command: ");
    kernel::serial::write(argv[0]);
    kernel::serial::write("\n");
}

bool keyboard_read_char_nonblocking(char* out_char) {
    if (out_char == nullptr) {
        return false;
    }

    if ((kernel::arch::x86::io::inb(0x64) & 0x01u) == 0) {
        return false;
    }

    const uint8_t scancode = kernel::arch::x86::io::inb(0x60);

    if (scancode & 0x80u) {
        return false;
    }

    switch (scancode) {
        case 0x1C:
            *out_char = '\n';
            return true;
        case 0x0E:
            *out_char = '\b';
            return true;
        case 0x39:
            *out_char = ' ';
            return true;
        case 0x02: *out_char = '1'; return true;
        case 0x03: *out_char = '2'; return true;
        case 0x04: *out_char = '3'; return true;
        case 0x05: *out_char = '4'; return true;
        case 0x06: *out_char = '5'; return true;
        case 0x07: *out_char = '6'; return true;
        case 0x08: *out_char = '7'; return true;
        case 0x09: *out_char = '8'; return true;
        case 0x0A: *out_char = '9'; return true;
        case 0x0B: *out_char = '0'; return true;
        case 0x10: *out_char = 'q'; return true;
        case 0x11: *out_char = 'w'; return true;
        case 0x12: *out_char = 'e'; return true;
        case 0x13: *out_char = 'r'; return true;
        case 0x14: *out_char = 't'; return true;
        case 0x15: *out_char = 'y'; return true;
        case 0x16: *out_char = 'u'; return true;
        case 0x17: *out_char = 'i'; return true;
        case 0x18: *out_char = 'o'; return true;
        case 0x19: *out_char = 'p'; return true;
        case 0x1E: *out_char = 'a'; return true;
        case 0x1F: *out_char = 's'; return true;
        case 0x20: *out_char = 'd'; return true;
        case 0x21: *out_char = 'f'; return true;
        case 0x22: *out_char = 'g'; return true;
        case 0x23: *out_char = 'h'; return true;
        case 0x24: *out_char = 'j'; return true;
        case 0x25: *out_char = 'k'; return true;
        case 0x26: *out_char = 'l'; return true;
        case 0x2C: *out_char = 'z'; return true;
        case 0x2D: *out_char = 'x'; return true;
        case 0x2E: *out_char = 'c'; return true;
        case 0x2F: *out_char = 'v'; return true;
        case 0x30: *out_char = 'b'; return true;
        case 0x31: *out_char = 'n'; return true;
        case 0x32: *out_char = 'm'; return true;
        case 0x33: *out_char = ','; return true;
        case 0x34: *out_char = '.'; return true;
        case 0x35: *out_char = '/'; return true;
        case 0x27: *out_char = ';'; return true;
        case 0x28: *out_char = '\''; return true;
        case 0x29: *out_char = '`'; return true;
        case 0x1A: *out_char = '['; return true;
        case 0x1B: *out_char = ']'; return true;
        case 0x2B: *out_char = '\\'; return true;
        case 0x0C: *out_char = '-'; return true;
        case 0x0D: *out_char = '='; return true;
        default:
            return false;
    }
}

void console_loop(uint32_t multiboot_info_addr) {
    char line[kMaxLine] = {};
    char cwd[32] = {};
    uint32_t pos = 0;

    kernel::fs::g_fs = &g_ramfs;
    g_ramfs.reset();

    // Ensure a minimal kernel-root filesystem layout exists as a reliable fallback.
    if (kernel::fs::g_fs != nullptr) {
        kernel::fs::g_fs->mkdir("/home");
        kernel::fs::g_fs->mkdir("/root");
        kernel::fs::g_fs->mkdir("/user");
        kernel::fs::g_fs->mkdir("/temp");
    }

    RootfsModuleContext rootfs_ctx = {};
    kernel::boot::multiboot2::visit_modules(multiboot_info_addr, rootfs_module_visitor, &rootfs_ctx);

    if (rootfs_ctx.loaded) {
        kernel::serial::write("[wirth/x86_64] rootfs seed loaded\n");
    } else {
        kernel::serial::write("[wirth/x86_64] rootfs seed missing, trying embedded seed\n");
        if (kEmbeddedRootfsSeedSize > 0) {
            load_rootfs_seed(kEmbeddedRootfsSeed, kEmbeddedRootfsSeedSize);
            kernel::serial::write("[wirth/x86_64] embedded rootfs seed applied\n");
        } else {
            kernel::serial::write("[wirth/x86_64] no embedded rootfs seed\n");
        }
    }

    copy_text(cwd, "/home", sizeof(cwd));

    print_prompt(cwd);

    while (true) {
        char c = 0;

        if (!kernel::serial::read_char_nonblocking(&c)) {
            if (!keyboard_read_char_nonblocking(&c)) {
                asm volatile("pause");
                continue;
            }
        }

        if (c == '\r' || c == '\n') {
            kernel::serial::write("\n");
            line[pos] = '\0';

            char* argv[kMaxArgs] = {};
            const int argc = split_args(line, argv, static_cast<int>(kMaxArgs));
            execute_command(argc, argv, cwd);

            pos = 0;
            print_prompt(cwd);
            continue;
        }

        if (c == '\t') {
            uint32_t command_start = 0;
            while (command_start < pos && line[command_start] == ' ') {
                ++command_start;
            }

            bool path_token = false;
            for (uint32_t i = 0; i < pos; ++i) {
                if (line[i] == ' ' || line[i] == '\t') {
                    path_token = true;
                    break;
                }
            }

            bool completed = false;
            if (path_token) {
                completed = complete_path_token(line, pos, cwd);
            } else {
                completed = complete_command_token(line, pos);
            }

            if (!completed) {
                kernel::serial::write("\a");
            }

            continue;
        }

        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                --pos;
                kernel::serial::write("\b \b");
            }
            continue;
        }

        if (c < 32 || c > 126) {
            continue;
        }

        if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
            kernel::serial::write_char(c);
        }
    }
}

}  // namespace

extern "C" void kernel_main64(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    constexpr uint32_t kExpectedMagic = 0x36D76289;

    kernel::serial::init();
    kernel::serial::write("[wirth/x86_64] kernel boot\n");
    kernel::serial::write("[wirth/x86_64] multiboot magic: 0x");
    kernel::serial::write_hex(multiboot_magic);
    kernel::serial::write("\n");
    kernel::serial::write("[wirth/x86_64] mbi addr: 0x");
    kernel::serial::write_hex(multiboot_info_addr);
    kernel::serial::write("\n");

    if (multiboot_magic != kExpectedMagic) {
        kernel::serial::write("[wirth/x86_64] invalid multiboot magic\n");
        
    } else {
        kernel::serial::write("[wirth/x86_64] bootstrap OK\n");
    }

    kernel::boot::multiboot2::log_memory_map(multiboot_info_addr);
    kernel::boot::multiboot2::log_modules(multiboot_info_addr);
    kernel::arch::x86_64::gdt::init();
    kernel::arch::x86_64::interrupts::init();
    kernel::arch::x86_64::interrupts::enable();
    kernel::serial::write("[wirth/x86_64] scaffold live\n");

    console_loop(multiboot_info_addr);
}
