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
#include "video.hpp"
#include "storage.hpp"
#include "pci.hpp"
#include "drivers/ide.hpp"
#include "efifs.hpp"
#include "xhci.hpp"
#include "usb_mass_storage.hpp"

namespace {

constexpr uint32_t kMaxLine = 128;
constexpr uint32_t kMaxArgs = 8;
constexpr uint32_t kMaxCommands = 64;
constexpr size_t kKernelHeapSize = 1024u * 1024u;

alignas(16) static uint8_t g_kernel_heap[kKernelHeapSize] = {};
static size_t g_kernel_heap_used = 0;

static kernel::fs::RamFs* g_ramfs = nullptr;

// Console output helper: emit via serial forwards to VGA
static inline void out(const char* s) {
    if (s == nullptr) return;
    kernel::serial::write(s);
}

static inline void out_char(char c) {
    kernel::serial::write_char(c);
}

bool filesystem_ready();
void copy_text(char* dst, const char* src, uint32_t max_size);
bool text_equal(const char* a, const char* b);
bool text_starts_with(const char* text, const char* prefix);
bool path_equals(const char* a, const char* b);
bool is_digit(char c);
bool path_starts_with(const char* text, const char* prefix);
bool resolve_path(const char* cwd, const char* input, char* out, uint32_t out_size);
bool keyboard_read_char_nonblocking(char* out_char);

uint32_t text_len(const char* text);
const char* canonical_command_name(const char* name);
void* kernel_alloc(size_t size, size_t alignment);

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
    {"reboot", "reboot", 0}, {"poweroff", "poweroff", 0}, {"lsdisk", "lsdisk", 0},
    {"lscpu", "lscpu", 0}, {"lspci", "lspci", 0}, {"tree", "tree", 0},
    {"cp", "cp", 0}, {"mv", "mv", 0}, {"rm", "rm", 0}, {"rmdir", "rmdir", 0},
    {"cat", "cat", 0}, {"touch", "touch", 0}, {"stat", "stat", 0},
};

const char* basename_of(const char* path) {
    if (path == nullptr) return "";

    int last = -1;
    for (int i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '/'){
            last = i;
        } 
    }

    return (last < 0) ? path : (path + last + 1);
}

bool fs_copy_file(const char* src, const char* dst) {
    if (!filesystem_ready()) {
        out("fs unavailable\n");
        return false;
    }

    int in_fd = kernel::fs::g_fs->open(src, kernel::fs::kOpenRead);

    if (in_fd < 0) {
        out("cp: source not found\n");
        return false;
    }

    int out_fd = kernel::fs::g_fs->open(dst, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate);

    if (out_fd < 0) {
        out("cp: cannot create destination\n");
        kernel::fs::g_fs->close(in_fd);

        return false;
    }

    const uint32_t BUF_SZ = 256;
    uint8_t buf[BUF_SZ];

    while (true) {
        int n = kernel::fs::g_fs->read(in_fd, buf, BUF_SZ);
        if (n < 0) {
            out("cp: read error\n");

            kernel::fs::g_fs->close(in_fd);
            kernel::fs::g_fs->close(out_fd);

            return false;
        }

        if (n == 0) break;
        int w = kernel::fs::g_fs->write(out_fd, buf, static_cast<uint32_t>(n));

        if (w < 0) {
            out("cp: write error\n");

            kernel::fs::g_fs->close(in_fd);
            kernel::fs::g_fs->close(out_fd);

            return false;
        }
    }

    kernel::fs::g_fs->close(in_fd);
    kernel::fs::g_fs->close(out_fd);

    return true;
}

bool fs_move_file(const char* src, const char* dst) {
    if (!fs_copy_file(src, dst)) 
        return false;

    if (kernel::fs::g_fs->unlink(src) == 0) 
        return true;

    out("mv: could not remove source after copy\n");

    return false;
}

bool fs_remove_path(const char* path) {

    if (!filesystem_ready()) {
        kernel::serial::write("fs unavailable\n");
        return false;
    }

    if (kernel::fs::g_fs->unlink(path) == 0) 
        return true;

    if (kernel::fs::g_fs->rmdir(path) == 0) 
        return true;

    return false;
}

void print_file(const char* path) {
    
    if (!filesystem_ready()) {
        out("fs unavailable\n");
        return;
    }
    
    int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);
    
    if (fd < 0) {
        out("cat: file not found\n");
        return;
    }

    const uint32_t BUF_SZ = 128;
    char buf[BUF_SZ + 1];
    
    while (true) {
        int n = kernel::fs::g_fs->read(fd, buf, BUF_SZ);
    
        if (n < 0) { out("cat: read error\n"); break; }
        if (n == 0) break;
    
        buf[n] = '\0';
        out(buf);
    }

    kernel::fs::g_fs->close(fd);
}

void print_tree_recursive(const char* path, int depth) {

    if (!filesystem_ready()) return;
    
    kernel::fs::DirEntry entries[32] = {};

    int count = kernel::fs::g_fs->readdir(path, entries, 32);
    
    if (count < 0) return;

    for (int i = 0; i < count; ++i) {
    
        for (int d = 0; d < depth; ++d) out("  ");
    
        out(entries[i].name);
        out(entries[i].is_directory ? "/\n" : "\n");

        if (entries[i].is_directory) {
            
            char child[128] = {};

            if (text_equal(path, "/")) {
                child[0] = '/';

                uint32_t pos = 1;
                uint32_t j = 0;

                while (entries[i].name[j] != '\0' && pos + 1 < sizeof(child)) {
                    child[pos++] = entries[i].name[j++];
                }

                child[pos] = '\0';

            } else {

                uint32_t pos = 0;
                uint32_t j = 0;

                while (path[j] != '\0' && pos + 2 < sizeof(child)) {
                    child[pos++] = path[j++];
                }

                if (pos + 2 < sizeof(child)) {
                    child[pos++] = '/';
                }

                j = 0;

                while (entries[i].name[j] != '\0' && pos + 1 < sizeof(child)) {
                    child[pos++] = entries[i].name[j++];
                }

                child[pos] = '\0';
            }

            print_tree_recursive(child, depth + 1);
        }
    }
}

__attribute__((unused)) void show_lsdisk() {
    uint8_t iddata[512] = {};

    if (!kernel::drivers::ide_identify(iddata)) {
        out("lsdisk: no IDE disk present\n");
        return;
    }

    // model string is in words 27..46 (40 bytes), each word is byte-swapped
    char model[41] = {};
    for (int w = 27; w <= 46; ++w) {
        const int base = (w - 27) * 2;
        model[base + 0] = static_cast<char>(iddata[w * 2 + 1]);
        model[base + 1] = static_cast<char>(iddata[w * 2 + 0]);
    }

    // trim trailing spaces
    for (int i = 39; i >= 0; --i) {
        if (model[i] == ' ' || model[i] == '\0') {
            model[i] = '\0';
        } else {
            break;
        }
    }

    // total user-addressable sectors (28-bit) at words 60..61 (offset 120)
    uint32_t sectors = *(uint32_t*)(iddata + 120);
    uint32_t kib = sectors / 2; // sectors*512 / 1024 = sectors/2

    out("NAME\tSIZE\tTYPE\n");
    out("hd0\t");
    // write_dec is not available here; use serial helpers
    kernel::serial::write_hex(kib);
    out(" KiB\t disk\n");

    out("MODEL: ");
    out(model);
    out("\n");

    // attempt to read MBR and list partitions
    uint8_t mbr[512] = {};
    if (!kernel::drivers::ide_read_sectors(0, mbr, 1)) {
        out("lsdisk: cannot read MBR\n");
        return;
    }

    // Partition table entries at offset 446, 4 entries of 16 bytes
    out("PARTS:\n");
    for (int i = 0; i < 4; ++i) {
        const uint8_t* pe = mbr + 446 + i * 16;
        (void)pe[0];
        uint8_t part_type = pe[4];
        uint32_t start_lba = *(uint32_t*)(pe + 8);
        uint32_t length = *(uint32_t*)(pe + 12);

        if (part_type == 0 || length == 0) continue;

        out("  ");
        // print partition number and type
        kernel::serial::write("part");
        kernel::serial::write_hex((uint32_t)(i+1));
        out(": ");
        out("type=0x");
        kernel::serial::write_hex(part_type);
        out(" start=");
        kernel::serial::write_hex(start_lba);
        out(" sectors=");
        kernel::serial::write_hex(length);
        out("\n");
    }
}

void show_lscpu() {
    // cpuid brand string (extended leaves 0x80000002..0x80000004)
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    // basic info
    out("Architecture: x86_64\n");
    out("CPU(s): 1\n");

    // extended max
    uint32_t maxext = 0;
    asm volatile("cpuid" : "=a"(maxext) : "a"(0x80000000) : "ebx","ecx","edx");

    if (maxext >= 0x80000004u) {
        char brand[49] = {};
        uint32_t regs[4];

        for (uint32_t i = 0; i < 3; ++i) {
            asm volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(0x80000002u + i));
            *(uint32_t*)(brand + i * 16 + 0) = regs[0];
            *(uint32_t*)(brand + i * 16 + 4) = regs[1];
            *(uint32_t*)(brand + i * 16 + 8) = regs[2];
            *(uint32_t*)(brand + i * 16 + 12) = regs[3];
        }

        brand[48] = '\0';
        out("Model: ");
        out(brand);
        out("\n");
    } else {
        out("Model: (unknown)\n");
    }
}

void show_lspci() {
    kernel::pci::Device devs[64];
    uint32_t found = 0;

    if (!kernel::pci::scan_devices(devs, 64, &found) || found == 0) {
        out("lspci: no devices found\n");
        return;
    }

    for (uint32_t i = 0; i < found; ++i) {
        const kernel::pci::Device& d = devs[i];

        // format: bb:ss.f vendor/device class
        kernel::serial::write_hex((uint32_t)d.bus);
        out(":");
        kernel::serial::write_hex((uint32_t)d.slot);
        out(".");
        kernel::serial::write_hex((uint32_t)d.func);
        out(" ");

        out("vendor=0x"); kernel::serial::write_hex(d.vendor_id);
        out(" device=0x"); kernel::serial::write_hex(d.device_id);
        out(" class=0x"); kernel::serial::write_hex(d.class_code);
        out(" sub=0x"); kernel::serial::write_hex(d.subclass);
        out(" progif=0x"); kernel::serial::write_hex(d.prog_if);
        out(" irq="); kernel::serial::write_hex(d.irq);

        for (int b = 0; b < 6; ++b) {
            if (d.bar[b] != 0u) {
                out(" BAR"); kernel::serial::write_hex((uint32_t)b);
                out("=0x"); kernel::serial::write_hex(d.bar[b]);
            }
        }

        out("\n");
    }
}

typedef int (*CmdHandler)(int argc, char* argv[], char* cwd);

struct CmdEntry {
    char name[32];
    CmdHandler handler;
};

static CmdEntry g_cmd_table[1024] = {};
static uint32_t g_cmd_count = 0;

bool register_command(const char* name, CmdHandler handler) {
    if (name == nullptr || handler == nullptr) return false;

    for (uint32_t i = 0; i < g_cmd_count; ++i) {

        if (text_equal(g_cmd_table[i].name, name)) {
            g_cmd_table[i].handler = handler;

            return true;
        }
    }

    if (g_cmd_count >= sizeof(g_cmd_table) / sizeof(g_cmd_table[0])) return false;

    copy_text(g_cmd_table[g_cmd_count].name, name, sizeof(g_cmd_table[g_cmd_count].name));

    g_cmd_table[g_cmd_count].handler = handler;
    ++g_cmd_count;

    return true;
}

CmdHandler find_registered_command(const char* name) {
    if (name == nullptr) return nullptr;

    for (uint32_t i = 0; i < g_cmd_count; ++i) {
        if (text_equal(g_cmd_table[i].name, name)) return g_cmd_table[i].handler;
    }

    const char* canon = canonical_command_name(name);

    if (canon != nullptr && !text_equal(canon, name)) {

        for (uint32_t i = 0; i < g_cmd_count; ++i) {
        
            if (text_equal(g_cmd_table[i].name, canon)) 
                return g_cmd_table[i].handler;
        }
    }

    return nullptr;
}

int cmd_stub(int argc, char* argv[], char* cwd) {
    (void)argc; (void)argv; (void)cwd;

    if (argc <= 0 || argv == nullptr) return 0;

    out(argv[0]);
    out(": stub (not implemented)\n");

    return 0;
}

int cmd_echo(int argc, char* argv[], char* cwd) {
    (void)cwd;

    for (int i = 1; i < argc; ++i) {
        out(argv[i]);

        if (i + 1 < argc) out(" ");
    }

    out("\n");

    return 0;
}

int cmd_sync(int argc, char* argv[], char* cwd) {
    (void)argc; (void)argv; (void)cwd;

    if (kernel::storage_ready()) {
        out("sync: persisting rootfs...\n");
        kernel::storage_snapshot_save("rootfs");

    } else {
        out("sync: no storage available\n");

    }

    return 0;
}

int cmd_exit(int argc, char* argv[], char* cwd) {
    (void)argc; (void)argv; (void)cwd;

    if (kernel::storage_ready()) {
        out("persisting rootfs...\n");
        kernel::storage_snapshot_save("rootfs");
    }

    out("exiting shell\n");
    asm volatile("cli");

    for (;;) asm volatile("hlt");

    return 0;
}

int cmd_apt(int argc, char* argv[], char* cwd) {
    (void)cwd;

    if (argc < 2) {
        out("apt: usage: apt <install|remove|list> [pkg]\n");
        return 0;
    }

    if (text_equal(argv[1], "install")) {

        if (argc < 3) { out("apt: install <pkg>\n"); return 0; }
        out("apt: simulated install: ");
        
        out(argv[2]);
        out("\n");
        
        return 0;
    }

    if (text_equal(argv[1], "remove")) {
        if (argc < 3) { out("apt: remove <pkg>\n"); return 0; }
        
        out("apt: simulated remove: ");
        
        out(argv[2]);
        out("\n");
        
        return 0;
    }

    if (text_equal(argv[1], "list")) {
        out("apt: simulated package list (none)\n");
        return 0;
    }

        out("apt: unknown subcommand\n");
    return 0;
}

int cmd_nano(int argc, char* argv[], char* cwd) {
    if (argc < 2) {
        kernel::serial::write("nano: usage: nano <file>\n");
        return 0;
    }

    char path[64] = {};
    resolve_path(cwd, argv[1], path, sizeof(path));

    if (filesystem_ready()) {
        int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);
        
        if (fd >= 0) {
            const uint32_t BUF_SZ = 128;
            char buf[BUF_SZ + 1];
        
            while (true) {
                int n = kernel::fs::g_fs->read(fd, reinterpret_cast<uint8_t*>(buf), BUF_SZ);
        
                if (n <= 0) break;
        
                buf[n] = '\0';
        
                kernel::serial::write(buf);
            }
        
            kernel::fs::g_fs->close(fd);
            kernel::serial::write("\n--- end of file ---\n");
        }
    }

    kernel::serial::write("[nano] enter lines, '.' on its own line to save\n");

    const uint32_t MAX_BUF = 4096;
    char* buffer = reinterpret_cast<char*>(kernel_alloc(MAX_BUF, 16));

    if (buffer == nullptr) {
        kernel::serial::write("nano: out of memory\n");
        return 0;
    }

    uint32_t buf_pos = 0;

    char linebuf[256] = {};
    uint32_t linepos = 0;

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
            linebuf[linepos] = '\0';

            if (linepos == 1 && linebuf[0] == '.') {
                break;
            }

            for (uint32_t i = 0; i < linepos && buf_pos + 1 < MAX_BUF; ++i) {
                buffer[buf_pos++] = linebuf[i];
            }

            if (buf_pos + 1 < MAX_BUF) buffer[buf_pos++] = '\n';

            linepos = 0;
            continue;
        }

        if (c == '\b' || c == 0x7F) {

            if (linepos > 0) {
                --linepos;
                kernel::serial::write("\b \b");
            }

            continue;
        }

        if (c < 32 || c > 126) continue;

        if (linepos + 1 < sizeof(linebuf)) {

            linebuf[linepos++] = c;
            kernel::serial::write_char(c);
        }
    }


    if (filesystem_ready()) {
        int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate);

        if (fd >= 0) {

            if (buf_pos > 0) {
                kernel::fs::g_fs->write(fd, reinterpret_cast<const uint8_t*>(buffer), buf_pos);
            }

            kernel::fs::g_fs->close(fd);
            kernel::serial::write("nano: saved\n");

            return 0;
        }
    }

    kernel::serial::write("nano: failed to save\n");
    return 0;
}

// Bulk register default and stub commands
void register_default_commands() {
    register_command("echo", cmd_echo);
    register_command("exit", cmd_exit);
    register_command("apt", cmd_apt);
    register_command("nano", cmd_nano);
    register_command("sync", cmd_sync);

    // register 300 stubs: cmd000..cmd299
    for (int i = 0; i < 300; ++i) {
        char tmp[16] = {};
        int a = i / 100; int b = (i / 10) % 10; int c = i % 10;

        tmp[0] = 'c'; tmp[1] = 'm'; tmp[2] = 'd'; tmp[3] = (char)('0' + a);
        tmp[4] = (char)('0' + b); tmp[5] = (char)('0' + c); tmp[6] = '\0';

        register_command(tmp, cmd_stub);
    }
}


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
            copy_text(out, "/home/root", out_size);
            return true;
        }

        if (input[1] == '/') {
            copy_text(out, "/home/root", out_size);
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

    // seed lines are applied silently

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

    if (text_equal(command, "mkdir") || text_equal(command, "md")) {
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

    // load seed silently
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

    if (text_starts_with(cwd, "/home/")) {
        uint32_t i = 6;
        uint32_t j = i;
        while (cwd[j] != '\0' && cwd[j] != '/') ++j;

        if (cwd[j] == '\0') {
            copy_text(out, "~", out_size);
            normalize_special_path(out, out_size);
            return;
        }

        // build "~" + remainder starting at j (including the slash)
        if (out_size == 0) return;
        out[0] = '~';
        uint32_t pos = 1;
        uint32_t k = j;
        while (cwd[k] != '\0' && pos + 1 < out_size) {
            out[pos++] = cwd[k++];
        }
        out[pos] = '\0';
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

        out(name);
        out(" ");
        any = true;
    }

    // include registered commands
    for (uint32_t i = 0; i < g_cmd_count; ++i) {
        const char* name = g_cmd_table[i].name;
        if (name == nullptr || !text_starts_with(name, prefix)) continue;
        out(name);
        out(" ");
        any = true;
    }

    if (any) {
        out("\n");
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
                out_char(first_match[prefix_len - 1]);
            }
            return true;
        }

        out("\n");
        print_command_candidates(prefix);
        return true;
    }

    const uint32_t name_len = text_len(first_match);
    while (prefix_len < name_len && pos + 1 < kMaxLine) {
        line[pos++] = first_match[prefix_len++];
        out_char(first_match[prefix_len - 1]);
    }

    return true;
}

bool complete_path_token(char* line, uint32_t& pos, const char* cwd) {
    uint32_t start = pos;
    while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t') {
        --start;
    }

    if (start >= pos) return false;

    // token is the current word being completed (may include slashes)
    char token[64] = {};
    uint32_t token_len = 0;
    while (start + token_len < pos && token_len + 1 < sizeof(token)) {
        token[token_len] = line[start + token_len];
        ++token_len;
    }
    token[token_len] = '\0';

    // base is text after the last '/'
    uint32_t base_start = 0;
    for (uint32_t i = 0; i < token_len; ++i) if (token[i] == '/') base_start = i + 1;
    const char* base = token + base_start;
    const uint32_t base_len = text_len(base);

    char resolved[128] = {};
    if (!resolve_path(cwd, token, resolved, sizeof(resolved))) return false;

    char parent[128] = {};
    if (!get_parent_path(resolved, parent, sizeof(parent))) return false;

    kernel::fs::DirEntry entries[64] = {};
    const int read_count = filesystem_ready() ? kernel::fs::g_fs->readdir(parent, entries, 64) : -1;
    if (read_count < 0) return false;

    const char* first_match = nullptr;
    bool first_is_dir = false;
    uint32_t match_count = 0;
    uint32_t common_len = 0;

    for (int i = 0; i < read_count; ++i) {
        const char* name = entries[i].name;
        if (base_len > 0 && !text_starts_with(name, base)) continue;

        ++match_count;
        if (first_match == nullptr) {
            first_match = name;
            first_is_dir = entries[i].is_directory;
            common_len = text_len(name);
            continue;
        }

        uint32_t shared = 0;
        while (first_match[shared] != '\0' && name[shared] != '\0' && first_match[shared] == name[shared]) ++shared;
        if (shared < common_len) common_len = shared;
    }

    if (match_count == 0) return false;

    if (match_count == 1 && first_match != nullptr) {
        // append remaining characters of the matched name
        const uint32_t name_len = text_len(first_match);
        for (uint32_t i = base_len; i < name_len && pos + 1 < kMaxLine; ++i) {
            char ch = first_match[i];
            line[pos++] = ch;
            out_char(ch);
        }

        // if directory, append trailing '/'
        if (first_is_dir && pos + 1 < kMaxLine) {
            line[pos++] = '/';
            out_char('/');
        }

        return true;
    }

    // multiple matches: print suggestions
    out("\n");
    for (int i = 0; i < read_count; ++i) {
        if (base_len > 0 && !text_starts_with(entries[i].name, base)) continue;
        out(entries[i].name);
        if (entries[i].is_directory) out("/");
        out("  ");
    }
    out("\n");
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
    out("\n");
    out("root@wirth:");
    out(prompt_path);
    out("$ ");
}

void print_directory_listing(const char* path) {
    if (!filesystem_ready()) {
        out("fs unavailable\n");
        return;
    }

    kernel::fs::DirEntry entries[32] = {};
    const int count = kernel::fs::g_fs->readdir(path, entries, 32);
    if (count < 0) {
        kernel::serial::write("ld: directory not found\n");
        return;
    }

    if (count == 0) {
        out("\n");
        return;
    }

    for (int i = 0; i < count; ++i) {
        out(entries[i].name);
        out("\n");
    }
}

void execute_command(int argc, char* argv[], char* cwd) {
    if (argc <= 0 || argv == nullptr || cwd == nullptr) {
        return;
    }

    CmdHandler reg = find_registered_command(argv[0]);

    if (reg != nullptr) {
        reg(argc, argv, cwd);
        return;
    }

    const char* command_name = canonical_command_name(argv[0]);

    if (command_name != nullptr && (text_equal(command_name, "clear") || text_equal(command_name, "cls") ||
        text_equal(command_name, "reset") || text_equal(command_name, "clean") || text_equal(command_name, "wipe") ||
        text_equal(command_name, "screen"))) {
    
        kernel::video::clear();
        kernel::serial::write("\x1b[2J\x1b[H");

        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "pwd") || text_equal(command_name, "cwd") ||
        text_equal(command_name, "whereami") || text_equal(command_name, "here"))) {

        out(cwd);        
        out("\n");
        
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
            out("cd: directory not found\n");
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
            out("ld: directory not found\n");
            return;
        }

        print_directory_listing(path);
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "md") || text_equal(command_name, "mkdir") ||
        text_equal(command_name, "mk") || text_equal(command_name, "makedir") || text_equal(command_name, "newdir") ||
        text_equal(command_name, "createdir") || text_equal(command_name, "create-dir"))) {

        if (argc < 2) {
            out("md <dir>\n");
            return;
        }

        char path[32] = {};
        resolve_path(cwd, argv[1], path, sizeof(path));

        if (path[0] == '\0') {
            out("md: invalid path\n");
            return;
        }

        if (filesystem_ready() && kernel::fs::g_fs->mkdir(path) == 0) {
            return;
        }

        if (directory_exists(path)) {
            out("md: directory already exists\n");
            return;
        }

        out("md: could not create directory\n");
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "reboot")) {
        out("rebooting...\n");

        if (kernel::storage_ready()) {
            out("persisting rootfs...\n");
            kernel::storage_snapshot_save("rootfs");
        }

        kernel::arch::x86::io::outb(0x64, 0xFE);

        for (;;){ asm volatile("cli; hlt"); }
    }

    if (command_name != nullptr && text_equal(command_name, "poweroff")) {
        out("powering off...\n");

        if (kernel::storage_ready()) {
            out("persisting rootfs...\n");
            kernel::storage_snapshot_save("rootfs");
        }

        asm volatile("cli");
        for (;;) asm volatile("hlt");

        return;
    }

    if (command_name != nullptr && text_equal(command_name, "lsblk")) {
        show_lspci();
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "lscpu")) {
        show_lscpu();
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "lspci")) {
        show_lspci();
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "tree")) {
        char path[64] = {};
        
        if (argc < 2) copy_text(path, cwd, sizeof(path)); else resolve_path(cwd, argv[1], path, sizeof(path));
        
        out(path);
        out("\n");
        print_tree_recursive(path, 0);
        
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "cp")) {
    
        if (argc < 3) { out("cp <src> <dst>\n"); return; }
        char src[64] = {};
        char dst[64] = {};
    
        resolve_path(cwd, argv[1], src, sizeof(src));
        resolve_path(cwd, argv[2], dst, sizeof(dst));
    
        if (text_equal(dst, "/") ) { out("cp: invalid destination\n"); return; }
    
        if (directory_exists(dst)) {
            char final_dst[128] = {};
            uint32_t pos = 0; uint32_t j = 0;
    
            while (dst[j] != '\0' && pos + 2 < sizeof(final_dst)) final_dst[pos++] = dst[j++];
    
            if (final_dst[pos - 1] != '/') { if (pos + 1 < sizeof(final_dst)) final_dst[pos++] = '/'; }
    
            const char* base = basename_of(src);
            j = 0; 
            
            while (base[j] != '\0' && pos + 1 < sizeof(final_dst)) final_dst[pos++] = base[j++];
            
            final_dst[pos] = '\0';
            
            if (!fs_copy_file(src, final_dst)) out("cp: failed\n");
            
            return;
        }
        
        if (!fs_copy_file(src, dst)) out("cp: failed\n");
        
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "mv")) {
        
        if (argc < 3) { out("mv <src> <dst>\n"); return; }
        
        char src[64] = {};
        char dst[64] = {};
        
        resolve_path(cwd, argv[1], src, sizeof(src));
        resolve_path(cwd, argv[2], dst, sizeof(dst));
        
        if (directory_exists(dst)) {
            char final_dst[128] = {};
            uint32_t pos = 0; uint32_t j = 0;
        
            while (dst[j] != '\0' && pos + 2 < sizeof(final_dst)) final_dst[pos++] = dst[j++];
        
            if (final_dst[pos - 1] != '/') { if (pos + 1 < sizeof(final_dst)) final_dst[pos++] = '/'; }
        
            const char* base = basename_of(src);
            j = 0; 
            
            while (base[j] != '\0' && pos + 1 < sizeof(final_dst)) final_dst[pos++] = base[j++];
            
            final_dst[pos] = '\0';
            
            if (!fs_move_file(src, final_dst)) out("mv: failed\n");
            
            return;
        }
        
        if (!fs_move_file(src, dst)) out("mv: failed\n");
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "rm")) {
        if (argc < 2) { out("rm <path>\n"); return; }
        
        char path[64] = {};
        
        resolve_path(cwd, argv[1], path, sizeof(path));
        
        if (fs_remove_path(path)) return;
        
        out("rm: failed\n");
        
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "rmdir")) {
        
        if (argc < 2) { out("rmdir <dir>\n"); return; }
        
        char path[64] = {};
        
        resolve_path(cwd, argv[1], path, sizeof(path));
        
        if (kernel::fs::g_fs->rmdir(path) == 0) return;
        
        out("rmdir: failed\n");
        
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "cat")) {
        
        if (argc < 2) { out("cat <file>\n"); return; }
        
        char path[64] = {};
        
        resolve_path(cwd, argv[1], path, sizeof(path));
        
        print_file(path);
        
        out("\n");
        
        return;
    }
    
    if (command_name != nullptr && text_equal(command_name, "touch")) {
        
        if (argc < 2) { out("touch <file>\n"); return; }
        
        char path[64] = {};
        
        resolve_path(cwd, argv[1], path, sizeof(path));
        
        int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate);
        
        if (fd < 0) { out("touch: failed\n"); return; }
        
        kernel::fs::g_fs->close(fd);
        
        return;
    }

    if (command_name != nullptr && text_equal(command_name, "stat")) {
        
        if (argc < 2) { out("stat <path>\n"); return; }
        
        char path[64] = {};
        
        resolve_path(cwd, argv[1], path, sizeof(path));
        
        if (directory_exists(path)) { out("directory\n"); return; }
        
        int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);
        
        if (fd >= 0) { out("file\n"); kernel::fs::g_fs->close(fd); return; }
        
        out("not found\n");
        
        return;
    }

    if (command_name != nullptr && (text_equal(command_name, "help") || text_equal(command_name, "?") ||
        text_equal(command_name, "h") || text_equal(command_name, "commands") || text_equal(command_name, "cmds") ||
        text_equal(command_name, "apropos") || text_equal(command_name, "usage"))) {
        
        out("commands: ld, md, cd, pwd, clear, plus numbered families like ld12/md7\n");
        return;
    }

    out("unknown command: ");
    out(argv[0]);
    out("\n");
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
        case 0x1C: *out_char = '\n'; return true;
        case 0x0E: *out_char = '\b'; return true;
        case 0x39: *out_char = ' '; return true;
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

    if (g_ramfs == nullptr) {
        g_ramfs = new kernel::fs::RamFs();
    }

    kernel::storage_init();
    kernel::fs::g_fs = g_ramfs;
    (void)g_ramfs->init();

    kernel::storage_restore_packages();

    register_default_commands();

    RootfsModuleContext rootfs_ctx = {};
    kernel::boot::multiboot2::visit_modules(multiboot_info_addr, rootfs_module_visitor, &rootfs_ctx);

    if (!rootfs_ctx.loaded) {
        kernel::serial::write("[wirth] xhci probe\n");
        const bool xhci_ready = kernel::xhci::init();
        kernel::serial::write(xhci_ready ? "[wirth] xhci ready\n" : "[wirth] xhci unavailable\n");

        if (xhci_ready) {
            kernel::xhci::start_poll_task();
        }

        kernel::serial::write("[wirth] usbms probe\n");
        const bool usbms_ready = kernel::usbms::init();
        kernel::serial::write(usbms_ready ? "[wirth] usbms ready\n" : "[wirth] usbms unavailable\n");

        if (!kernel::efifs::try_load_rootfs_seed_from_esp()) {
            kernel::serial::write("[efifs]: failed to load ROOTFS.SEED from ESP\n");

            const char* seed = kEmbeddedRootfsSeed;
            const unsigned seed_sz = kEmbeddedRootfsSeedSize;

            char linebuf[128];
            uint32_t line_len = 0;

            for (unsigned i = 0; i < seed_sz; ++i) {
                char ch = seed[i];

                if (ch == '\r' || ch == '\n') {

                    if (line_len > 0) {
                        linebuf[line_len] = '\0';
                        char* cursor = linebuf;
                    
                        while (*cursor == ' ' || *cursor == '\t') 
                            ++cursor;
                    
                        if (*cursor != '\0' && *cursor != '#') {
                            char* cmd = cursor;
                    
                            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') 
                                ++cursor;
                    
                            if (*cursor != '\0') *cursor++ = '\0';
                    
                            while (*cursor == ' ' || *cursor == '\t') 
                                ++cursor;
                    
                            if (*cmd == 'm' && cmd[1] == 'd') {
                                kernel::fs::g_fs->mkdir(cursor);
                            }
                        }
                    
                        line_len = 0;
                    }
                    
                    continue;
                }

                if (line_len + 1 < sizeof(linebuf)) linebuf[line_len++] = ch;
            }

            if (line_len > 0) {
                linebuf[line_len] = '\0';
                char* cursor = linebuf;
                
                while (*cursor == ' ' || *cursor == '\t') 
                    ++cursor;
                
                if (*cursor != '\0' && *cursor != '#') {
                    char* cmd = cursor;
                
                    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') 
                        ++cursor;
                
                    if (*cursor != '\0') *cursor++ = '\0';
                
                    while (*cursor == ' ' || *cursor == '\t') 
                        ++cursor;
                
                    if (*cmd == 'm' && cmd[1] == 'd') {
                        kernel::fs::g_fs->mkdir(cursor);
                    }
                }
            }
        }
    }
    copy_text(cwd, "/home/root", sizeof(cwd));

    kernel::serial::write("Hello from wirth!\n");
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
    kernel::serial::write("[wirth] kernel boot\n");
    kernel::serial::write("[wirth] multiboot magic: 0x");
    kernel::serial::write_hex(multiboot_magic);
    kernel::serial::write("\n");
    kernel::serial::write("[wirth] mbi addr: 0x");
    kernel::serial::write_hex(multiboot_info_addr);
    kernel::serial::write("\n");

    if (multiboot_magic != kExpectedMagic) {
        kernel::serial::write("[wirth] invalid multiboot magic\n");
        
    } else {
        kernel::serial::write("[wirth] bootstrap OK\n");
    }

    kernel::boot::multiboot2::log_memory_map(multiboot_info_addr);
    kernel::boot::multiboot2::log_modules(multiboot_info_addr);

    kernel::arch::x86_64::gdt::init();
    kernel::arch::x86_64::interrupts::init();

    kernel::arch::x86_64::interrupts::enable();
    
    console_loop(multiboot_info_addr);
}
