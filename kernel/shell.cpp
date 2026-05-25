#include "shell.hpp"

#include <stdint.h>

#include "arch/x86/io.hpp"
#include "arch/x86/interrupts.hpp"
#include "fs/vfs.hpp"
#include "mm/heap.hpp"
#include "mm/pmm.hpp"
#include "serial.hpp"
#include "task/scheduler.hpp"
#include "storage.hpp"
#include "block.hpp"
#include "pci.hpp"
#include "drivers/ide.hpp"
#include "ahci.hpp"
#include "xhci.hpp"
#include "usb_mass_storage.hpp"

namespace {

constexpr uint32_t kMaxLine = 192;
constexpr uint32_t kMaxArgs = 24;
constexpr uint32_t kMaxPath = 64;
constexpr uint32_t kFileChunk = 128;
constexpr uint32_t kMaxTreeDepth = 8;
constexpr uint32_t kHistoryCapacity = 16;

constexpr const char kAnsiReset[] = "\x1b[0m";
constexpr const char kAnsiBoldCyan[] = "\x1b[1;36m";
constexpr const char kAnsiGreen[] = "\x1b[1;32m";
constexpr const char kAnsiYellow[] = "\x1b[1;33m";
constexpr const char kAnsiRed[] = "\x1b[1;31m";
constexpr const char kKernelName[] = "wirthOS";
constexpr const char kKernelNodeName[] = "wirth";
constexpr const char kKernelRelease[] = "0.2";
constexpr const char kKernelVersion[] = "kernel-serial-shell";
constexpr const char kKernelMachine[] = "i686";
constexpr const char kKernelProcessor[] = "i686";
constexpr const char kKernelHardware[] = "x86";
constexpr const char kKernelBuildStamp[] = __DATE__ " " __TIME__;

struct ShellContext {
    char cwd[kMaxPath];
    bool running;
};

static char g_history[kHistoryCapacity][kMaxLine];

static uint32_t g_history_count = 0;
static int32_t g_history_view = -1;

static char g_history_draft[kMaxLine] = {};
static bool g_history_draft_valid = false;

using CommandHandler = void (*)(ShellContext&, int, char*[]);

struct CommandEntry {
    const char* name;
    CommandHandler handler;
};

extern const CommandEntry kCommands[];
extern const uint32_t kCommandCount;

bool resolve_path(const char* cwd, const char* input, char* out, uint32_t out_size);
const char* suggest_command(const char* name);

uint32_t text_len(const char* s) {
    uint32_t n = 0;

    while (s[n] != '\0') {
        ++n;
    }

    return n;

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

uint32_t min3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t m = (a < b) ? a : b;
    return (m < c) ? m : c;
}

uint32_t edit_distance(const char* a, const char* b) {
    constexpr uint32_t kMaxWord = 32;

    const uint32_t len_a = text_len(a);
    const uint32_t len_b = text_len(b);

    if (len_a > kMaxWord || len_b > kMaxWord) {
        return len_a + len_b;
    }

    uint32_t prev[kMaxWord + 1] = {};
    uint32_t curr[kMaxWord + 1] = {};

    for (uint32_t j = 0; j <= len_b; ++j) {
        prev[j] = j;
    }

    for (uint32_t i = 1; i <= len_a; ++i) {
        curr[0] = i;

        for (uint32_t j = 1; j <= len_b; ++j) {
            const uint32_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            curr[j] = min3(prev[j] + 1u, curr[j - 1] + 1u, prev[j - 1] + cost);
        }

        for (uint32_t j = 0; j <= len_b; ++j) {
            prev[j] = curr[j];
        }
    }

    return prev[len_b];
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

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

void write_dec(uint32_t value) {
    char buf[11];
    uint32_t i = 0;

    if (value == 0) {
        kernel::serial::write("0");
        return;
    }

    while (value > 0 && i < sizeof(buf)) {
        buf[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        kernel::serial::write_char(buf[--i]);
    }
}

void write_line(const char* text) {
    kernel::serial::write(text);
    kernel::serial::write("\n");
}

[[maybe_unused]] void write_colored_line(const char* color, const char* text) {
    kernel::serial::write(color);
    kernel::serial::write(text);
    kernel::serial::write(kAnsiReset);
    kernel::serial::write("\n");
}

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(leaf), "c"(subleaf));
}

const char* pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x10EC: return "Realtek";
        case 0x1234: return "QEMU/Bochs";
        case 0x1B36: return "Red Hat/QEMU";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD/ATI";
        default: return nullptr;
    }
}

const char* pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x01: return "mass storage (IDE)";
                case 0x06: return "mass storage (SATA)";
                case 0x08: return "mass storage (NVMe)";
                default: return "mass storage";
            }
        case 0x02:
            return "network controller";
        case 0x03:
            return "display controller";
        case 0x04:
            return "multimedia controller";
        case 0x05:
            return "memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI bridge";
                default: return "bridge";
            }
        case 0x0C:
            if (subclass == 0x03) {
                switch (prog_if) {
                    case 0x00: return "USB controller (UHCI)";
                    case 0x10: return "USB controller (OHCI)";
                    case 0x20: return "USB controller (EHCI)";
                    case 0x30: return "USB controller (xHCI)";
                    default: return "USB controller";
                }
            }
            return "serial bus controller";
        default:
            return "unknown";
    }
}

void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

[[noreturn]] void machine_reboot() {
    asm volatile("cli");

    while ((kernel::arch::x86::io::inb(0x64) & 0x02u) != 0);

    kernel::arch::x86::io::outb(0x64, 0xFE);

    for (;;) {
        asm volatile("hlt");
    }
}

[[noreturn]] void machine_poweroff() {
    asm volatile("cli");

    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    for (;;) {
        asm volatile("hlt");
    }
}

void print_prompt(const ShellContext& ctx) {
    kernel::serial::write(kAnsiBoldCyan);
    kernel::serial::write("root@wirth:");
    kernel::serial::write(kAnsiGreen);
    kernel::serial::write(ctx.cwd);
    kernel::serial::write(kAnsiReset);
    kernel::serial::write("# ");
}

void redraw_input(const ShellContext& ctx, const char* line, uint32_t pos) {
    kernel::serial::write("\r\x1b[2K");
    print_prompt(ctx);

    for (uint32_t i = 0; i < pos; ++i) {

        kernel::serial::write_char(line[i]);
    }
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

uint32_t history_size() {
    return (g_history_count < kHistoryCapacity) ? g_history_count : kHistoryCapacity;
}

const char* history_at(uint32_t index) {
    const uint32_t size = history_size();

    if (index >= size || size == 0) {
        return nullptr;
    }

    const uint32_t start = (g_history_count < kHistoryCapacity) ? 0u : (g_history_count % kHistoryCapacity);
    const uint32_t slot = (start + index) % kHistoryCapacity;

    return g_history[slot];
}

void history_append(const char* line) {

    if (line == nullptr || line[0] == '\0') {
        return;
    }

    if (g_history_count > 0) {
        const char* last = history_at(history_size() - 1);

        if (last != nullptr && text_equal(last, line)) {
            return;
        }
    }

    const uint32_t slot = g_history_count % kHistoryCapacity;

    copy_text(g_history[slot], line, kMaxLine);
    ++g_history_count;
}

void history_reset_view() {
    g_history_view = -1;
    g_history_draft_valid = false;
    g_history_draft[0] = '\0';
}

bool history_move_up(const ShellContext& ctx, char* line, uint32_t& pos) {

    const uint32_t size = history_size();

    if (size == 0) {
        return false;
    }

    if (g_history_view < 0) {
        copy_text(g_history_draft, line, sizeof(g_history_draft));
        g_history_draft_valid = true;
        g_history_view = static_cast<int32_t>(size - 1u);

    } else if (g_history_view > 0) {
        --g_history_view;
    }

    const char* entry = history_at(static_cast<uint32_t>(g_history_view));

    if (entry == nullptr) {
        return false;
    }

    copy_text(line, entry, kMaxLine);
    pos = text_len(line);

    redraw_input(ctx, line, pos);

    return true;
}

bool history_move_down(const ShellContext& ctx, char* line, uint32_t& pos) {

    const uint32_t size = history_size();

    if (size == 0 || g_history_view < 0) {
        return false;
    }

    if (static_cast<uint32_t>(g_history_view) + 1u < size) {
        ++g_history_view;

        const char* entry = history_at(static_cast<uint32_t>(g_history_view));

        if (entry == nullptr) {
            return false;
        }

        copy_text(line, entry, kMaxLine);

    } else {
        g_history_view = -1;

        if (g_history_draft_valid) {
            copy_text(line, g_history_draft, kMaxLine);

        } else {
            line[0] = '\0';
        }
    }

    pos = text_len(line);
    redraw_input(ctx, line, pos);

    return true;
}

uint32_t common_prefix_len(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return 0;
    }

    uint32_t i = 0;

    while (a[i] != '\0' && b[i] != '\0' && a[i] == b[i]) {
        ++i;
    }

    return i;
}

void write_candidate_list(const char* title, const char* const* candidates, uint32_t count) {

    if (title == nullptr || candidates == nullptr || count == 0) {
        return;
    }

    kernel::serial::write("\n");
    kernel::serial::write(title);
    kernel::serial::write(": ");

    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) {
            kernel::serial::write(", ");
        }

        kernel::serial::write(candidates[i]);
    }

    kernel::serial::write("\n");
}

void write_dir_candidate_list(const char* title, const kernel::fs::DirEntry* entries, uint32_t count) {
    if (title == nullptr || entries == nullptr || count == 0) {
        return;
    }

    kernel::serial::write("\n");
    kernel::serial::write(title);
    kernel::serial::write(": ");

    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) {
            kernel::serial::write(", ");
        }

        kernel::serial::write(entries[i].name);

        if (entries[i].is_directory != 0) {
            kernel::serial::write("/");
        }
    }

    kernel::serial::write("\n");
}

bool complete_path_token(const ShellContext& ctx, char* line, uint32_t& pos) {
    uint32_t token_start = pos;

    while (token_start > 0 && !is_space(line[token_start - 1])) {
        --token_start;
    }

    if (token_start >= pos) {
        return false;
    }

    const char* token = &line[token_start];

    uint32_t token_len = pos - token_start;
    uint32_t last_slash = token_len;

    for (uint32_t i = 0; i < token_len; ++i) {

        if (token[i] == '/') {
            last_slash = i;
        }
    }

    char parent[kMaxPath];
    char prefix[kMaxPath];
    char replacement[kMaxPath];

    replacement[0] = '\0';

    if (last_slash == token_len) {
        copy_text(parent, ctx.cwd, sizeof(parent));
        copy_text(prefix, token, sizeof(prefix));

    } else {

        if (last_slash == 0) {
            copy_text(parent, "/", sizeof(parent));

        } else {
            char partial[kMaxPath];

            uint32_t copy_len = (last_slash < sizeof(partial)) ? last_slash : (sizeof(partial) - 1u);

            for (uint32_t i = 0; i < copy_len; ++i) {
                partial[i] = token[i];
            }

            partial[copy_len] = '\0';

            if (!resolve_path(ctx.cwd, partial, parent, sizeof(parent))) {
                return false;
            }
        }

        copy_text(prefix, token + last_slash + 1u, sizeof(prefix));
    }

    if (prefix[0] == '\0' && last_slash != token_len) {
        return false;
    }

    kernel::fs::DirEntry entries[32] = {};

    const int n = kernel::fs::g_fs->readdir(parent, entries, 32);

    if (n <= 0) {
        return false;
    }

    const kernel::fs::DirEntry* match = nullptr;

    kernel::fs::DirEntry matches[32] = {};
    uint32_t match_count = 0;
    uint32_t common_len = 0;

    for (int i = 0; i < n; ++i) {

        if (!text_starts_with(entries[i].name, prefix)) {
            continue;
        }

        if (match_count < 32) {
            matches[match_count] = entries[i];
        }

        ++match_count;

        if (match == nullptr) {
            match = &entries[i];
            common_len = text_len(entries[i].name);

        } else {
            const uint32_t shared = common_prefix_len(match->name, entries[i].name);
            common_len = (shared < common_len) ? shared : common_len;
        }
    }

    if (match == nullptr) {
        return false;
    }

    if (match_count > 1) {
        const uint32_t prefix_len = text_len(prefix);

        if (common_len > prefix_len) {
            uint32_t out_len = 0;

            if (last_slash != token_len) {

                for (uint32_t i = 0; i <= last_slash && out_len + 1 < sizeof(replacement); ++i) {
                    replacement[out_len++] = token[i];
                }
            }
            const char* candidate = match->name;

            for (uint32_t i = prefix_len; i < common_len && out_len + 1 < sizeof(replacement); ++i) {
                replacement[out_len++] = candidate[i];
            }

            replacement[out_len] = '\0';

            const uint32_t replacement_len = text_len(replacement);

            if (token_start + replacement_len < kMaxLine) {

                char original[kMaxLine];
                copy_text(original, line, sizeof(original));

                uint32_t tail_start = token_start + token_len;
                uint32_t tail_len = pos - tail_start;
                uint32_t out = 0;

                for (uint32_t i = 0; i < token_start && out + 1 < kMaxLine; ++i) {
                    line[out++] = original[i];
                }

                for (uint32_t i = 0; i < replacement_len && out + 1 < kMaxLine; ++i) {
                    line[out++] = replacement[i];
                }

                for (uint32_t i = 0; i < tail_len && out + 1 < kMaxLine; ++i) {
                    line[out++] = original[tail_start + i];
                }

                pos = out;
                line[pos] = '\0';

                redraw_input(ctx, line, pos);
            }

            return true;
        }

        write_dir_candidate_list("tab matches", matches, (match_count < 32) ? match_count : 32u);
        redraw_input(ctx, line, pos);

        return true;
    }

    uint32_t out_len = 0;

    if (last_slash != token_len) {
        for (uint32_t i = 0; i <= last_slash && out_len + 1 < sizeof(replacement); ++i) {
            replacement[out_len++] = token[i];
        }
    }

    const char* candidate = match->name;

    uint32_t cand_len = text_len(candidate);

    for (uint32_t i = prefix[0] == '\0' ? 0u : 
        text_len(prefix); i < cand_len && out_len + 1 < sizeof(replacement); ++i
    ) {

        replacement[out_len++] = candidate[i];
    }

    if (match->is_directory != 0 && out_len + 1 < sizeof(replacement)) {
        replacement[out_len++] = '/';
    }

    replacement[out_len] = '\0';

    uint32_t replacement_len = text_len(replacement);

    if (token_start + replacement_len >= kMaxLine) {
        return false;
    }

    char original[kMaxLine];

    copy_text(original, line, sizeof(original));

    uint32_t tail_start = token_start + token_len;
    uint32_t tail_len = pos - tail_start;
    uint32_t out = 0;

    for (uint32_t i = 0; i < token_start && out + 1 < kMaxLine; ++i) {
        line[out++] = original[i];
    }

    for (uint32_t i = 0; i < replacement_len && out + 1 < kMaxLine; ++i) {
        line[out++] = replacement[i];
    }

    for (uint32_t i = 0; i < tail_len && out + 1 < kMaxLine; ++i) {
        line[out++] = original[tail_start + i];
    }

    pos = out;
    line[pos] = '\0';

    redraw_input(ctx, line, pos);

    return true;
}

bool normalize_absolute_path(const char* in, char* out, uint32_t out_size) {

    if (in == nullptr || out == nullptr || out_size < 2 || in[0] != '/') {
        return false;
    }

    const char* segments[16] = {};

    uint32_t seg_lens[16] = {};
    uint32_t seg_count = 0;
    uint32_t i = 1;

    while (in[i] != '\0') {

        while (in[i] == '/') {
            ++i;
        }

        if (in[i] == '\0') {
            break;
        }

        const uint32_t start = i;

        while (in[i] != '\0' && in[i] != '/') {
            ++i;
        }

        const uint32_t len = i - start;

        if (len == 1 && in[start] == '.') {
            continue;
        }

        if (len == 2 && in[start] == '.' && in[start + 1] == '.') {

            if (seg_count > 0) {
                --seg_count;
            }

            continue;
        }

        if (seg_count >= 16) {
            return false;
        }

        segments[seg_count] = in + start;
        seg_lens[seg_count] = len;

        ++seg_count;
    }

    uint32_t out_i = 0;
    out[out_i++] = '/';

    if (seg_count == 0) {
        out[out_i] = '\0';
        return true;
    }

    for (uint32_t s = 0; s < seg_count; ++s) {

        if (s != 0) {

            if (out_i + 1 >= out_size) {

                return false;
            }

            out[out_i++] = '/';
        }

        if (out_i + seg_lens[s] >= out_size) {
            return false;
        }

        for (uint32_t j = 0; j < seg_lens[s]; ++j) {
            out[out_i++] = segments[s][j];
        }
    }

    out[out_i] = '\0';

    return true;
}

bool resolve_path(const char* cwd, const char* input, char* out, uint32_t out_size) {

    if (cwd == nullptr || input == nullptr || out == nullptr || out_size < 2) {

        return false;
    }

    char merged[kMaxPath];

    if (input[0] == '/') {
        copy_text(merged, input, sizeof(merged));

    } else if (text_equal(cwd, "/")) {
        merged[0] = '/';
        copy_text(merged + 1, input, sizeof(merged) - 1);

    } else {
        copy_text(merged, cwd, sizeof(merged));
        const uint32_t len = text_len(merged);

        if (len + 1 >= sizeof(merged)) {
            return false;
        }

        merged[len] = '/';
        merged[len + 1] = '\0';

        copy_text(merged + len + 1, input, sizeof(merged) - len - 1);
    }

    return normalize_absolute_path(merged, out, out_size);
}

bool arg_to_path(const ShellContext& ctx, const char* arg, char* out_path, const char* usage) {
    if (arg == nullptr) {
        kernel::serial::write(usage);
        kernel::serial::write("\n");
        return false;
    }

    if (!resolve_path(ctx.cwd, arg, out_path, kMaxPath)) {
        kernel::serial::write("invalid path\n");
        return false;
    }

    return true;
}

bool parse_u32(const char* s, uint32_t* out) {
    if (s == nullptr || out == nullptr || s[0] == '\0') {
        return false;
    }

    uint32_t value = 0;
    uint32_t i = 0;

    while (s[i] != '\0') {

        if (s[i] < '0' || s[i] > '9') {
            return false;
        }

        value = value * 10u + static_cast<uint32_t>(s[i] - '0');
        ++i;
    }

    *out = value;
    return true;
}

[[maybe_unused]] const CommandEntry* find_command_prefix(const char* prefix) {

    if (prefix == nullptr || prefix[0] == '\0') {
        return nullptr;
    }

    const CommandEntry* match = nullptr;

    for (uint32_t i = 0; i < kCommandCount; ++i) {
        const char* name = kCommands[i].name;
        uint32_t j = 0;

        while (prefix[j] != '\0' && name[j] != '\0' && prefix[j] == name[j]) {
            ++j;
        }

        if (prefix[j] != '\0') {
            continue;
        }

        if (match != nullptr) {
            return nullptr;
        }

        match = &kCommands[i];
    }

    return match;
}

void complete_command_line(ShellContext& ctx, char* line, uint32_t& pos) {

    if (pos == 0) {
        return;
    }
    uint32_t start = 0;

    while (start < pos && is_space(line[start])) {
        ++start;
    }

    if (start >= pos) {
        return;
    }

    uint32_t end = start;

    while (end < pos && !is_space(line[end])) {
        ++end;
    }

    char prefix[64] = {};
    uint32_t len = 0;

    while (start + len < pos && start + len < end && len + 1 < sizeof(prefix)) {
        prefix[len] = line[start + len];
        ++len;
    }
    prefix[len] = '\0';

    if (start > 0 || end != pos) {
        complete_path_token(ctx, line, pos);
        return;
    }

    const CommandEntry* matches[32] = {};

    uint32_t match_count = 0;
    uint32_t common_len = 0;

    for (uint32_t i = 0; i < kCommandCount; ++i) {
        const char* name = kCommands[i].name;
        uint32_t j = 0;

        while (prefix[j] != '\0' && name[j] != '\0' && prefix[j] == name[j]) {
            ++j;
        }

        if (prefix[j] != '\0') {
            continue;
        }

        if (match_count < 32) {
            matches[match_count] = &kCommands[i];
        }

        ++match_count;

        if (common_len == 0) {
            common_len = text_len(name);

        } else {
            const uint32_t shared = common_prefix_len(matches[0]->name, name);
            common_len = (shared < common_len) ? shared : common_len;
        }
    }

    if (match_count == 0) {
        const char* suggestion = suggest_command(prefix);

        if (suggestion != nullptr) {
            kernel::serial::write("\n");
            kernel::serial::write(kAnsiYellow);
            kernel::serial::write("tab: try ");
            kernel::serial::write(suggestion);
            kernel::serial::write(kAnsiReset);
            kernel::serial::write("\n");
        }

        redraw_input(ctx, line, pos);

        return;
    }

    if (match_count > 1) {
        if (common_len > len) {
            const char* name = matches[0]->name;

            for (uint32_t i = len; i < common_len && pos + 1 < kMaxLine; ++i) {
                line[pos++] = name[i];
                kernel::serial::write_char(name[i]);
            }

            return;
        }

        const char* names[32] = {};

        for (uint32_t i = 0; i < match_count && i < 32; ++i) {
            names[i] = matches[i]->name;
        }

        write_candidate_list("tab matches", names, (match_count < 32) ? match_count : 32u);
        redraw_input(ctx, line, pos);

        return;
    }

    const char* name = matches[0]->name;
    const uint32_t name_len = text_len(name);

    if (name_len > len) {

        for (uint32_t i = len; i < name_len && pos + 1 < kMaxLine; ++i) {

            line[pos++] = name[i];
            kernel::serial::write_char(name[i]);
        }
    }

    if (pos + 1 < kMaxLine) {
        line[pos++] = ' ';
        kernel::serial::write_char(' ');
    }
}

int parse_args(char* line, char* argv[kMaxArgs]) {
    int argc = 0;
    uint32_t i = 0;

    while (line[i] != '\0') {

        while (is_space(line[i])) {
            ++i;
        }

        if (line[i] == '\0') {
            break;
        }

        if (argc >= static_cast<int>(kMaxArgs)) {
            break;
        }

        if (line[i] == '"' || line[i] == '\'') {
            const char quote = line[i++];
            argv[argc++] = &line[i];

            while (line[i] != '\0' && line[i] != quote) {
                ++i;
            }

            if (line[i] == quote) {
                line[i++] = '\0';
            }

        } else {
            argv[argc++] = &line[i];

            while (line[i] != '\0' && !is_space(line[i])) {
                ++i;
            }

            if (line[i] != '\0') {
                line[i++] = '\0';
            }
        }

        while (is_space(line[i])) {
            ++i;
        }

        if (line[i] == '\0') {
            break;
        }

        if (line[i] == '#') {
            // shell-style inline comment
            line[i] = '\0';
            break;
        }

        if (is_space(line[i])) {
            ++i;
        }

        if (line[i] == '\0') {
            break;
        }

        if (line[i] == '"' || line[i] == '\'' || !is_space(line[i])) {
            // continue scanning next argument
            continue;
        }

        if (is_space(line[i])) {

            while (is_space(line[i])) {
                ++i;
            }

            if (line[i] == '\0') {
                break;
            }
        }
    }

    return argc;
}

void print_path_lines(const char* path, uint32_t max_lines, bool from_end) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");

        return;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        kernel::serial::write("file not found\n");

        return;
    }

    char buffer[1024];
    uint32_t n_total = 0;

    while (n_total < sizeof(buffer) - 1) {
        const int n = kernel::fs::g_fs->read(fd, &buffer[n_total], sizeof(buffer) - 1 - n_total);

        if (n <= 0) {
            break;
        }

        n_total += static_cast<uint32_t>(n);
    }

    kernel::fs::g_fs->close(fd);
    buffer[n_total] = '\0';

    if (!from_end) {
        uint32_t lines = 0;

        for (uint32_t i = 0; i < n_total; ++i) {
            kernel::serial::write_char(buffer[i]);

            if (buffer[i] == '\n') {
                ++lines;

                if (lines >= max_lines) {
                    break;
                }
            }
        }

        return;
    }

    uint32_t seen = 0;
    int32_t start = static_cast<int32_t>(n_total) - 1;

    while (start >= 0) {

        if (buffer[start] == '\n') {
            ++seen;

            if (seen > max_lines) {
                ++start;
                break;
            }
        }

        --start;
    }

    if (start < 0) {
        start = 0;
    }

    for (uint32_t i = static_cast<uint32_t>(start); i < n_total; ++i) {
        kernel::serial::write_char(buffer[i]);
    }
}

bool get_file_size(const char* path, uint32_t* out_size) {

    if (kernel::fs::g_fs == nullptr || out_size == nullptr) {
        return false;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        return false;
    }

    uint32_t size = 0;

    char buf[kFileChunk];

    while (true) {
        const int n = kernel::fs::g_fs->read(fd, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }

        size += static_cast<uint32_t>(n);
    }

    kernel::fs::g_fs->close(fd);
    *out_size = size;

    return true;
}

bool path_exists(const char* path) {

    if (kernel::fs::g_fs == nullptr || path == nullptr) {
        return false;
    }

    kernel::fs::DirEntry probe[1] = {};
    
    if (kernel::fs::g_fs->readdir(path, probe, 1) >= 0) {
        return true;
    }
    
    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);
    
    if (fd < 0) {
        return false;
    }
    
    kernel::fs::g_fs->close(fd);
    
    return true;
}

bool is_directory(const char* path) {
    if (kernel::fs::g_fs == nullptr || path == nullptr) {
        return false;
    }
    
    kernel::fs::DirEntry probe[1] = {};
    
    return kernel::fs::g_fs->readdir(path, probe, 1) >= 0;
}

bool is_regular_file(const char* path) {
    
    if (kernel::fs::g_fs == nullptr || path == nullptr) {
        return false;
    }
    
    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);
    
    if (fd < 0) {
        return false;
    }
    
    kernel::fs::g_fs->close(fd);
    
    return true;
}

const char* path_basename(const char* path) {
    
    if (path == nullptr || path[0] == '\0') {
        return "";
    }
    
    const uint32_t len = text_len(path);
    
    if (len == 1 && path[0] == '/') {
        return path;
    }
    
    int32_t i = static_cast<int32_t>(len) - 1;
    
    while (i > 0 && path[i] == '/') {
        --i;
    }
    
    while (i > 0 && path[i - 1] != '/') {
        --i;
    }
    
    return &path[i];
}

void path_dirname(const char* path, char* out, uint32_t out_size) {
    
    if (path == nullptr || out == nullptr || out_size < 2) {
        return;
    }
    
    const uint32_t len = text_len(path);
    
    if (len == 0 || (len == 1 && path[0] == '/')) {
        copy_text(out, "/", out_size);
        return;
    }
    
    int32_t i = static_cast<int32_t>(len) - 1;
    
    while (i > 0 && path[i] == '/') {
        --i;
    }
    
    while (i > 0 && path[i] != '/') {
        --i;
    }
    
    if (i <= 0) {
        copy_text(out, "/", out_size);
        return;
    }
    
    uint32_t n = static_cast<uint32_t>(i);
    
    if (n >= out_size) {
        n = out_size - 1;
    }
    
    for (uint32_t k = 0; k < n; ++k) {
        out[k] = path[k];
    }
    
    out[n] = '\0';
}

void join_rest_args(int argc, char* argv[], int start, char* out, uint32_t out_size) {
    
    if (out == nullptr || out_size == 0) {
        return;
    }
    
    uint32_t w = 0;
    out[0] = '\0';
    
    for (int i = start; i < argc; ++i) {
        const char* s = argv[i];
    
        if (s == nullptr) {
            continue;
        }
    
        if (i > start && w + 1 < out_size) {
            out[w++] = ' ';
        }
    
        for (uint32_t j = 0; s[j] != '\0' && w + 1 < out_size; ++j) {
            out[w++] = s[j];
        }
    }
    
    out[w] = '\0';
}

bool write_file_text(const char* path, const char* text, bool append_mode) {
    if (kernel::fs::g_fs == nullptr || path == nullptr || text == nullptr) {
        return false;
    }
    
    const uint32_t flags_base = kernel::fs::kOpenWrite | kernel::fs::kOpenCreate;
    const uint32_t flags =
        append_mode ? (flags_base | kernel::fs::kOpenRead) : (flags_base | kernel::fs::kOpenTruncate);
    
    const int fd = kernel::fs::g_fs->open(path, flags);
    
    if (fd < 0) {
        return false;
    }
    
    if (append_mode) {
        char sink[kFileChunk];
        while (kernel::fs::g_fs->read(fd, sink, sizeof(sink)) > 0) {}
    }
    
    const uint32_t n = text_len(text);
    const int wr = kernel::fs::g_fs->write(fd, text, n);
    
    kernel::fs::g_fs->close(fd);
    
    return wr >= 0;
}

bool path_exists(const char* path);

struct PackageFileEntry {
    const char* path;
    const char* contents;
};

struct PackageDefinition {
    const char* name;
    const char* version;
    const char* summary;
    const PackageFileEntry* files;
    uint32_t file_count;
};

constexpr const char kPkgDbDir[] = "/var/lib/wirth/pkgdb";
constexpr const char kAptDir[] = "/etc/apt";
constexpr const char kAptSourcesDir[] = "/etc/apt/sources.list.d";
constexpr const char kDocDir[] = "/usr/share/doc/wirth";

constexpr PackageFileEntry kBaseSystemFiles[] = {
    {"/etc/issue", "wirth OS - package-managed shell build\n"},
    {"/etc/apt/sources.list.d/wirth.list", "deb local://wirth stable main\n"},
    {"/usr/share/doc/wirth/base-system.txt", "Base system metadata package.\n"},
};

constexpr PackageFileEntry kHomeLayoutFiles[] = {
    {"/root/README.txt", "Root home workspace managed by wirth packages.\n"},
    {"/home/root/README.txt", "Primary home directory for the root user.\n"},
    {"/home/root/Documents/README.txt", "Documents folder seeded by the package manager.\n"},
    {"/home/root/Downloads/README.txt", "Downloads folder seeded by the package manager.\n"},
    {"/home/root/Desktop/README.txt", "Desktop folder seeded by the package manager.\n"},
};

constexpr PackageFileEntry kDevToolsFiles[] = {
    {"/usr/share/doc/wirth/devtools.txt", "Developer tools and shell inspection helpers.\n"},
    {"/usr/share/doc/wirth/apt.txt", "wirth apt-like package manager.\n"},
    {"/usr/share/doc/wirth/commands.txt", "Shell command families and aliases.\n"},
};

constexpr PackageFileEntry kDemoDocsFiles[] = {
    {"/opt/demo/hello.txt", "Hello from a package-installed demo payload.\n"},
    {"/opt/demo/notes.txt", "This package proves file installation works.\n"},
};

constexpr PackageDefinition kPackages[] = {
    {"base-system", "1.0", "Base system metadata and apt bootstrap", kBaseSystemFiles,
     static_cast<uint32_t>(sizeof(kBaseSystemFiles) / sizeof(kBaseSystemFiles[0]))},
    {"home-layout", "1.0", "Default home layout and starter docs", kHomeLayoutFiles,
     static_cast<uint32_t>(sizeof(kHomeLayoutFiles) / sizeof(kHomeLayoutFiles[0]))},
    {"devtools", "1.0", "Developer docs and package inspection helpers", kDevToolsFiles,
     static_cast<uint32_t>(sizeof(kDevToolsFiles) / sizeof(kDevToolsFiles[0]))},
    {"demo-docs", "1.0", "Demo payload installed under /opt", kDemoDocsFiles,
     static_cast<uint32_t>(sizeof(kDemoDocsFiles) / sizeof(kDemoDocsFiles[0]))},
};

constexpr uint32_t kPackageCount = static_cast<uint32_t>(sizeof(kPackages) / sizeof(kPackages[0]));

bool contains_text(const char* text, const char* needle) {
    
    if (text == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    
    const uint32_t text_len_value = text_len(text);
    
    const uint32_t needle_len = text_len(needle);
    
    if (needle_len > text_len_value) {
        return false;
    }
    
    for (uint32_t i = 0; i + needle_len <= text_len_value; ++i) {
        uint32_t j = 0;

        while (j < needle_len && text[i + j] == needle[j]) {
            ++j;
        }

        if (j == needle_len) {
            return true;
        }
    }

    return false;
}

bool ensure_dir(const char* path) {
    if (kernel::fs::g_fs == nullptr || path == nullptr) {
        return false;
    }

    if (path_exists(path)) {
        return true;
    }

    return kernel::fs::g_fs->md(path) >= 0;
}

bool ensure_package_dirs() {
    return ensure_dir("/var") && ensure_dir("/var/lib") && ensure_dir("/var/lib/wirth") &&
           ensure_dir(kPkgDbDir) && ensure_dir(kAptDir) && ensure_dir(kAptSourcesDir) &&
           ensure_dir("/usr") && ensure_dir("/usr/share") && ensure_dir("/usr/share/doc") &&
           ensure_dir(kDocDir) && ensure_dir("/opt") && ensure_dir("/opt/demo");
}

const PackageDefinition* find_package(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }

    for (uint32_t i = 0; i < kPackageCount; ++i) {

        if (text_equal(kPackages[i].name, name)) {
            return &kPackages[i];
        }
    }

    return nullptr;
}

void build_package_marker(const char* pkg_name, char* out, uint32_t out_size) {
    copy_text(out, kPkgDbDir, out_size);

    const uint32_t len = text_len(out);

    if (len + 2 >= out_size) {
        return;
    }

    if (out[len - 1] != '/') {
        out[len] = '/';
        out[len + 1] = '\0';
    }

    const uint32_t after_slash = text_len(out);
    uint32_t w = after_slash;

    for (uint32_t i = 0; pkg_name[i] != '\0' && w + 1 < out_size; ++i) {
        out[w++] = pkg_name[i];
    }

    if (w + 12 >= out_size) {
        out[0] = '\0';
        return;
    }

    out[w++] = '.';
    out[w++] = 'i';
    out[w++] = 'n';
    out[w++] = 's';
    out[w++] = 't';
    out[w++] = 'a';
    out[w++] = 'l';
    out[w++] = 'l';
    out[w++] = 'e';
    out[w++] = 'd';
    out[w] = '\0';
}

bool package_installed(const PackageDefinition& pkg) {
    char marker[96] = {};
    build_package_marker(pkg.name, marker, sizeof(marker));

    return marker[0] != '\0' && path_exists(marker);
}

bool install_package_files(const PackageDefinition& pkg) {

    for (uint32_t i = 0; i < pkg.file_count; ++i) {

        if (!write_file_text(pkg.files[i].path, pkg.files[i].contents, false)) {
            return false;
        }
    }

    return true;
}

void remove_package_files(const PackageDefinition& pkg) {
    if (kernel::fs::g_fs == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < pkg.file_count; ++i) {
        kernel::fs::g_fs->unlink(pkg.files[i].path);
    }

    char marker[96] = {};

    build_package_marker(pkg.name, marker, sizeof(marker));

    if (marker[0] != '\0') {
        kernel::fs::g_fs->unlink(marker);
    }
}

bool install_package(const PackageDefinition& pkg) {

    if (!ensure_package_dirs()) {
        return false;
    }

    if (package_installed(pkg)) {
        return true;
    }

    if (!install_package_files(pkg)) {
        remove_package_files(pkg);
        return false;
    }

    // persist package payload to disk and register in index
    // build binary payload: [path_len(uint32)][data_len(uint32)][path bytes][data bytes]...
    uint32_t total = 0;

    for (uint32_t i = 0; i < pkg.file_count; ++i) {
        const char* path = pkg.files[i].path;
        const char* contents = pkg.files[i].contents;

        uint32_t path_len = text_len(path);
        uint32_t data_len = text_len(contents);

        total += 8 + path_len + data_len;
    }

    uint8_t* payload = reinterpret_cast<uint8_t*>(kernel::mm::heap::alloc(total, 4096));

    if (payload == nullptr) {
        remove_package_files(pkg);
        return false;
    }

    uint32_t off = 0;

    for (uint32_t i = 0; i < pkg.file_count; ++i) {
        const char* path = pkg.files[i].path;
        const char* contents = pkg.files[i].contents;

        uint32_t path_len = text_len(path);
        uint32_t data_len = text_len(contents);

        *(uint32_t*)(payload + off) = path_len;
        *(uint32_t*)(payload + off + 4) = data_len;

        off += 8;

        for (uint32_t k = 0; k < path_len; ++k) payload[off + k] = path[k];
        off += path_len;

        for (uint32_t k = 0; k < data_len; ++k) payload[off + k] = contents[k];
        off += data_len;
    }

    kernel::StorageHandle h = {};
    if (!kernel::storage_write(payload, total, &h)) {
        (void)payload; // heap has no free
        remove_package_files(pkg);

        return false;
    }

    if (!kernel::storage_add_index_entry(pkg.name, h.start_lba, h.sectors)) {
        (void)payload; // heap has no free
        remove_package_files(pkg);
        return false;
    }

    (void)payload; // heap has no free

    char marker[96] = {};

    build_package_marker(pkg.name, marker, sizeof(marker));

    if (marker[0] == '\0' || !write_file_text(marker, pkg.version, false)) {
        remove_package_files(pkg);

        return false;
    }

    return true;
}

bool package_match(const PackageDefinition& pkg, const char* needle) {

    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }

    return contains_text(pkg.name, needle) || contains_text(pkg.summary, needle);
}

const PackageDefinition* suggest_package(const char* name) {
    const PackageDefinition* best = nullptr;
    uint32_t best_score = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < kPackageCount; ++i) {
        const uint32_t score = edit_distance(name, kPackages[i].name);

        if (score < best_score) {
            best_score = score;
            best = &kPackages[i];
        }
    }

    if (best == nullptr) {
        return nullptr;
    }

    if (best_score <= 3u) {
        return best;
    }

    return nullptr;
}

void print_package_info(const PackageDefinition& pkg) {
    kernel::serial::write(pkg.name);
    kernel::serial::write(" ");

    kernel::serial::write(pkg.version);
    kernel::serial::write(" - ");

    kernel::serial::write(pkg.summary);
    kernel::serial::write("\n");
}

void print_package_files(const PackageDefinition& pkg) {

    for (uint32_t i = 0; i < pkg.file_count; ++i) {
        kernel::serial::write("  ");
        kernel::serial::write(pkg.files[i].path);
        kernel::serial::write("\n");
    }
}

void print_package_status(const PackageDefinition& pkg) {
    kernel::serial::write(package_installed(pkg) ? "installed " : "available ");
    print_package_info(pkg);
}

const char* suggest_from_list(const char* name, const char* const* items, uint32_t item_count) {
    const char* best = nullptr;
    uint32_t best_score = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < item_count; ++i) {
        const uint32_t score = edit_distance(name, items[i]);

        if (score < best_score) {
            best_score = score;
            best = items[i];
        }
    }

    if (best != nullptr && best_score <= 3u) {
        return best;
    }

    return nullptr;
}

bool write_package_index() {

    if (!ensure_package_dirs()) {
        return false;
    }

    char index[1024] = {};
    uint32_t used = 0;

    for (uint32_t i = 0; i < kPackageCount; ++i) {
        const PackageDefinition& pkg = kPackages[i];

        const char* state = package_installed(pkg) ? "installed" : "available";
        const char* parts[] = {pkg.name, "|", pkg.version, "|", state, "|", pkg.summary, "\n"};

        for (uint32_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p) {
            const char* text = parts[p];

            for (uint32_t j = 0; text[j] != '\0' && used + 1 < sizeof(index); ++j) {
                index[used++] = text[j];
            }
        }
    }

    index[used] = '\0';

    return write_file_text("/var/lib/wirth/pkgdb/available.list", index, false);
}

void print_package_catalog(bool installed_only, bool available_only, const char* needle) {

    for (uint32_t i = 0; i < kPackageCount; ++i) {
        const PackageDefinition& pkg = kPackages[i];
        const bool installed = package_installed(pkg);

        if (installed_only && !installed) {
            continue;
        }

        if (available_only && installed) {
            continue;
        }

        if (!package_match(pkg, needle)) {
            continue;
        }

        print_package_status(pkg);
    }
}

void pkg(ShellContext&, int argc, char* argv[]) {
    static const char* kSubcommands[] = {"update", "search", "list", "show", "info", "install", "remove", "files", "policy", "upgrade"};

    if (argc < 2) {
        kernel::serial::write("usage: apt <update|search|list|show|install|remove|files|policy|upgrade>\n");
        return;
    }

    const char* sub = argv[1];

    if (text_equal(sub, "update")) {
        if (write_package_index()) {
            kernel::serial::write("apt: package lists updated\n");

        } else {
            kernel::serial::write("apt: update failed\n");
        }

        return;
    }

    if (text_equal(sub, "search")) {
        const char* needle = (argc >= 3) ? argv[2] : "";

        print_package_catalog(false, false, needle);

        return;
    }

    if (text_equal(sub, "list")) {
        const char* mode = (argc >= 3) ? argv[2] : "all";

        if (text_equal(mode, "installed")) {
            print_package_catalog(true, false, nullptr);

        } else if (text_equal(mode, "available")) {
            print_package_catalog(false, true, nullptr);

        } else {
            print_package_catalog(false, false, nullptr);

        }

        return;
    }

    if (text_equal(sub, "show") || text_equal(sub, "info") || text_equal(sub, "policy") || text_equal(sub, "files")) {

        if (argc < 3) {
            kernel::serial::write("apt: package name required\n");
            return;
        }

        const PackageDefinition* pkg = find_package(argv[2]);

        if (pkg == nullptr) {
            const char* suggestion = suggest_package(argv[2]) != nullptr ? suggest_package(argv[2])->name : nullptr;

            kernel::serial::write("apt: package not found: ");
            kernel::serial::write(argv[2]);
            kernel::serial::write("\n");

            if (suggestion != nullptr) {
                kernel::serial::write("do you mean package: ");
                kernel::serial::write(suggestion);
                kernel::serial::write("?\n");
            }

            return;
        }

        print_package_info(*pkg);
        kernel::serial::write("installed: ");
        kernel::serial::write(package_installed(*pkg) ? "yes\n" : "no\n");

        if (text_equal(sub, "policy")) {
            kernel::serial::write("candidate: ");
            kernel::serial::write(pkg->version);
            kernel::serial::write("\n");

            return;
        }

        if (text_equal(sub, "files")) {
            print_package_files(*pkg);

            return;
        }

        return;
    }

    if (text_equal(sub, "install") || text_equal(sub, "remove") || text_equal(sub, "upgrade")) {

        if (text_equal(sub, "upgrade")) {

            for (uint32_t i = 0; i < kPackageCount; ++i) {

                if (package_installed(kPackages[i])) {
                    remove_package_files(kPackages[i]);

                    if (!install_package(kPackages[i])) {
                        kernel::serial::write("apt: upgrade failed for ");
                        kernel::serial::write(kPackages[i].name);
                        kernel::serial::write("\n");
                    }
                }
            }

            kernel::serial::write("apt: upgrade complete\n");

            return;
        }

        if (argc < 3) {
            kernel::serial::write(text_equal(sub, "install") ? "apt install <pkg> [pkg...]\n" : "apt remove <pkg> [pkg...]\n");
            return;
        }

        for (int i = 2; i < argc; ++i) {
            const PackageDefinition* pkg = find_package(argv[i]);

            if (pkg == nullptr) {
                const char* suggestion = suggest_package(argv[i]) != nullptr ? suggest_package(argv[i])->name : nullptr;

                kernel::serial::write("apt: package not found: ");
                kernel::serial::write(argv[i]);
                kernel::serial::write("\n");

                if (suggestion != nullptr) {
                    kernel::serial::write("do you mean package: ");
                    kernel::serial::write(suggestion);
                    kernel::serial::write("?\n");
                }

                continue;

            }

            if (text_equal(sub, "install")) {

                if (install_package(*pkg)) {
                    kernel::serial::write("apt: installed ");
                    kernel::serial::write(pkg->name);
                    kernel::serial::write("\n");

                } else {
                    kernel::serial::write("apt: install failed for ");
                    kernel::serial::write(pkg->name);
                    kernel::serial::write("\n");
                }

            } else {
                remove_package_files(*pkg);

                kernel::serial::write("apt: removed ");
                kernel::serial::write(pkg->name);
                kernel::serial::write("\n");
            }
        }
        return;
    }

    const char* suggestion = suggest_from_list(sub, kSubcommands, static_cast<uint32_t>(sizeof(kSubcommands) / sizeof(kSubcommands[0])));

    kernel::serial::write("apt: unknown subcommand: ");
    kernel::serial::write(sub);
    kernel::serial::write("\n");
    
    if (suggestion != nullptr) {

        kernel::serial::write("do you mean: ");
        kernel::serial::write(suggestion);
        kernel::serial::write("?\n");
    }
}

void help(ShellContext&, int, char*[]);
void clear(ShellContext&, int, char*[]);
void whoami(ShellContext&, int, char*[]);
void id(ShellContext&, int, char*[]);
void pwd(ShellContext&, int, char*[]);
void cd(ShellContext&, int, char*[]);
void ld(ShellContext&, int, char*[]);
void md(ShellContext&, int, char*[]);
void rd(ShellContext&, int, char*[]);
void cat(ShellContext&, int, char*[]);
void touch(ShellContext&, int, char*[]);
void rm(ShellContext&, int, char*[]);
void echo(ShellContext&, int, char*[]);
void halt(ShellContext&, int, char*[]);
void hid(ShellContext&, int, char*[]);
void keyboard(ShellContext&, int, char*[]);
void cp(ShellContext&, int, char*[]);
void mv(ShellContext&, int, char*[]);
void head(ShellContext&, int, char*[]);
void tail(ShellContext&, int, char*[]);
void wc(ShellContext&, int, char*[]);
void cmp(ShellContext&, int, char*[]);
void stat(ShellContext&, int, char*[]);
void tree(ShellContext&, int, char*[]);
void env(ShellContext&, int, char*[]);
void uname(ShellContext&, int, char*[]);
void ctrue(ShellContext&, int, char*[]);
void cfalse(ShellContext&, int, char*[]);
void basename(ShellContext&, int, char*[]);
void dirname(ShellContext&, int, char*[]);
void exists(ShellContext&, int, char*[]);
void isfile(ShellContext&, int, char*[]);
void isdir(ShellContext&, int, char*[]);
void size(ShellContext&, int, char*[]);
void write(ShellContext&, int, char*[]);
void append(ShellContext&, int, char*[]);
void truncate(ShellContext&, int, char*[]);
void nl(ShellContext&, int, char*[]);
void hexdump(ShellContext&, int, char*[]);
void find(ShellContext&, int, char*[]);
void grep(ShellContext&, int, char*[]);
void sleep(ShellContext&, int, char*[]);
void tick(ShellContext&, int, char*[]);
void uptime(ShellContext&, int, char*[]);
void ps(ShellContext&, int, char*[]);
void meminfo(ShellContext&, int, char*[]);
void free(ShellContext&, int, char*[]);
void df(ShellContext&, int, char*[]);
void lsblk(ShellContext&, int, char*[]);
void sync(ShellContext&, int, char*[]);
void pathjoin(ShellContext&, int, char*[]);
void realpath(ShellContext&, int, char*[]);
void seq(ShellContext&, int, char*[]);
void repeat(ShellContext&, int, char*[]);
void catb(ShellContext&, int, char*[]);
void countdir(ShellContext&, int, char*[]);
void touchmany(ShellContext&, int, char*[]);
void rmmany(ShellContext&, int, char*[]);
void cpu(ShellContext&, int, char*[]);
void devices(ShellContext&, int, char*[]);
void pci(ShellContext&, int, char*[]);
void drivers(ShellContext&, int, char*[]);
void pkg(ShellContext&, int, char*[]);
void reboot(ShellContext&, int, char*[]);
void poweroff(ShellContext&, int, char*[]);
void exit_shell(ShellContext&, int, char*[]);
void history(ShellContext&, int, char*[]);
void ahci(ShellContext&, int, char*[]);

#define CMD(name, fn) \
    { name, fn }

const CommandEntry kCommands[] = {
    CMD("help", help), CMD("?", help), CMD("man", help), CMD("commands", help),
    CMD("cmds", help), CMD("apropos", help), CMD("usage", help), CMD("h", help),

    CMD("clear", clear), CMD("cls", clear), CMD("reset", clear), CMD("clean", clear),
    CMD("wipe", clear), CMD("screen", clear),

    CMD("whoami", whoami), CMD("iam", whoami), CMD("user", whoami), CMD("me", whoami),
    CMD("id", id), CMD("identity", id), CMD("uidgid", id),

    CMD("pwd", pwd), CMD("cwd", pwd), CMD("whereami", pwd), CMD("here", pwd),
    CMD("cd", cd), CMD("chdir", cd), CMD("changedir", cd), CMD("goto", cd),

    CMD("ld", ld), CMD("dir", ld), CMD("ll", ld), CMD("la", ld),
    CMD("l", ld), CMD("list", ld), CMD("listdir", ld), CMD("contents", ld),
    CMD("browse", ld), CMD("showdir", ld), CMD("showfiles", ld), CMD("list-directory", ld),

    CMD("md", md), CMD("mk", md), CMD("makedir", md),
    CMD("newdir", md), CMD("createdir", md), CMD("create-dir", md),

    CMD("rd", rd), CMD("rd", rd), CMD("rmd", rd), CMD("removedir", rd),
    CMD("deldir", rd), CMD("delete-dir", rd), CMD("remove-dir", rd),

    CMD("cat", cat), CMD("print", cat), CMD("type", cat), CMD("more", cat),
    CMD("less", cat), CMD("show", cat), CMD("view", cat), CMD("read", cat),
    CMD("display", cat), CMD("dump", cat), CMD("readfile", cat),

    CMD("touch", touch), CMD("create", touch), CMD("newfile", touch), CMD("mkfile", touch),
    CMD("createfile", touch), CMD("new-file", touch), CMD("create-file", touch),

    CMD("rm", rm), CMD("del", rm), CMD("erase", rm), CMD("delete", rm),
    CMD("unlink", rm), CMD("remove", rm), CMD("trash", rm),

    CMD("echo", echo), CMD("say", echo), CMD("speak", echo), CMD("printline", echo),
    CMD("printf", echo),

    CMD("cp", cp), CMD("copy", cp), CMD("duplicate", cp), CMD("dup", cp), CMD("clone", cp),
    CMD("mv", mv), CMD("move", mv), CMD("rename", mv), CMD("ren", mv),

    CMD("head", head), CMD("first", head), CMD("top", head),
    CMD("tail", tail), CMD("last", tail), CMD("bottom", tail),
    CMD("wc", wc), CMD("count", wc), CMD("metrics", wc),
    CMD("cmp", cmp), CMD("diff", cmp), CMD("compare", cmp),
    CMD("stat", stat), CMD("info", stat), CMD("fileinfo", stat),
    CMD("status", stat), CMD("details", stat),
    CMD("tree", tree), CMD("treedir", tree), CMD("lsr", tree),

    CMD("env", env), CMD("set", env), CMD("vars", env), CMD("printenv", env),
    CMD("uname", uname), CMD("ver", uname), CMD("version", uname), CMD("about", uname),
    CMD("sysinfo", uname), CMD("kernel", uname),
    CMD("cpu", cpu), CMD("cpuid", cpu), CMD("lscpu", cpu), CMD("hwinfo", cpu),
    CMD("lspci", pci), CMD("pci", pci), CMD("lshid", hid), CMD("lskb", keyboard),
    CMD("hid", hid), CMD("kbd", keyboard), CMD("keyboard", keyboard),
    CMD("ahci", ahci),
    CMD("devices", devices), CMD("devs", devices), CMD("lsdev", devices), CMD("lsdevices", devices),
    CMD("drivers", drivers), CMD("lsdrv", drivers), CMD("drvs", drivers), CMD("modules", drivers),
    CMD("history", history), CMD("hlist", history),
    CMD("df", df), CMD("disk", df), CMD("storage", df), CMD("lsdisk", lsblk), CMD("lsblk", lsblk), CMD("sync", sync),
    CMD("apt", pkg), CMD("apt-get", pkg), CMD("pkg", pkg), CMD("pkgmgr", pkg),

    CMD("basename", basename), CMD("base", basename),
    CMD("dirname", dirname), CMD("dirbase", dirname),
    CMD("exists", exists), CMD("test-exists", exists),
    CMD("isfile", isfile), CMD("test-file", isfile),
    CMD("isdir", isdir), CMD("test-dir", isdir),
    CMD("size", size), CMD("filesize", size),
    CMD("write", write), CMD("overwrite", write),
    CMD("append", append), CMD("appendfile", append),
    CMD("truncate", truncate), CMD("truncatefile", truncate),
    CMD("nl", nl), CMD("number-lines", nl),
    CMD("hexdump", hexdump), CMD("xxd", hexdump),
    CMD("find", find), CMD("walk", find),
    CMD("grep", grep), CMD("search", grep),
    CMD("sleep", sleep), CMD("pause", sleep),
    CMD("tick", tick), CMD("ticks", tick),
    CMD("uptime", uptime), CMD("up", uptime),
    CMD("ps", ps), CMD("proc", ps), CMD("tasks", ps),
    CMD("meminfo", meminfo), CMD("mem", meminfo), CMD("free", free),
    CMD("pathjoin", pathjoin), CMD("joinpath", pathjoin),
    CMD("realpath", realpath), CMD("normpath", realpath),
    CMD("seq", seq), CMD("range", seq),
    CMD("repeat", repeat), CMD("repeattext", repeat),
    CMD("catb", catb), CMD("catraw", catb),
    CMD("countdir", countdir), CMD("dircount", countdir),
    CMD("touchmany", touchmany), CMD("mkfiles", touchmany),
    CMD("rmmany", rmmany), CMD("rmfiles", rmmany),

    CMD("true", ctrue), CMD("yes", ctrue),
    CMD("false", cfalse), CMD("no", cfalse),

    CMD("halt", halt), CMD("exit", exit_shell), CMD("quit", exit_shell), CMD("logoff", exit_shell),
    CMD("shutdown", poweroff), CMD("stdn", poweroff), CMD("poweroff", poweroff), CMD("stop", halt), CMD("off", halt),
    CMD("powerdown", halt), CMD("reboot", reboot), CMD("restart", reboot), CMD("rbt", reboot),
    CMD("warmboot", reboot), CMD("reset-machine", reboot),
};

constexpr uint32_t kCommandCount = static_cast<uint32_t>(sizeof(kCommands) / sizeof(kCommands[0]));

const CommandEntry* find_command(const char* name) {
    
    for (uint32_t i = 0; i < kCommandCount; ++i) {
    
        if (text_equal(kCommands[i].name, name)) {
            return &kCommands[i];
        }
    }
    
    return nullptr;
}

const char* suggest_command(const char* name) {
    const char* best = nullptr;
    uint32_t best_score = 0xFFFFFFFFu;
    
    for (uint32_t i = 0; i < kCommandCount; ++i) {
        const uint32_t score = edit_distance(name, kCommands[i].name);
    
        if (score < best_score) {
            best_score = score;
            best = kCommands[i].name;
        }
    }
    
    if (best == nullptr) {
        return nullptr;
    }
    
    const uint32_t input_len = text_len(name);
    
    if (best_score <= 2u) {
        return best;
    }
    
    if (best_score <= 3u && input_len >= 4u) {
        return best;
    }
    
    return nullptr;
}

void help(ShellContext&, int argc, char* argv[]) {
    const char* group = (argc >= 2) ? argv[1] : nullptr;
    kernel::serial::write("shell commands loaded: ");
    
    write_dec(kCommandCount);
    kernel::serial::write("\n");
    kernel::serial::write("help [group]\n");
    kernel::serial::write("groups: core | fs | system | info | power\n");
    
    if (group == nullptr || text_equal(group, "all") || text_equal(group, "core")) {
        kernel::serial::write("core: ld dir cd pwd md/md rd/rd cat/print touch rm echo cp mv head tail wc cmp stat tree env uname id whoami clear\n");
    }
    
    if (group == nullptr || text_equal(group, "all") || text_equal(group, "fs")) {
        kernel::serial::write("fs: basename dirname exists isfile isdir size write append truncate nl hexdump find grep\n");
    }
    
    if (group == nullptr || text_equal(group, "all") || text_equal(group, "system") || text_equal(group, "info")) {
        kernel::serial::write("info: cpu cpuid lscpu hwinfo devices drivers meminfo free ps uptime tick sleep history uname [-a|-s|-n|-r|-v|-m]\n");
    }
    
    if (group == nullptr || text_equal(group, "all") || text_equal(group, "system") || text_equal(group, "storage")) {
        kernel::serial::write("storage: df disk storage sync\n");
    }
    
    if (group == nullptr || text_equal(group, "all") || text_equal(group, "packages") || text_equal(group, "pkg")) {
        kernel::serial::write("packages: apt/apt-get/pkg/pkgmgr update search list show install remove files policy upgrade\n");
    }
    
    if (group == nullptr || text_equal(group, "all") || text_equal(group, "power")) {
        kernel::serial::write("power: reboot restart warmboot poweroff shutdown halt stop off powerdown\n");
    }
}

void clear(ShellContext&, int, char*[]) {
    for (uint32_t i = 0; i < 30; ++i) {
        kernel::serial::write("\r\n");
    }
}

void whoami(ShellContext&, int, char*[]) {
    kernel::serial::write("root\n");
}

void id(ShellContext&, int, char*[]) {
    kernel::serial::write("uid=");
    kernel::serial::write_hex(kernel::task::scheduler::current_user_id());
    kernel::serial::write(" gid=");
    kernel::serial::write_hex(kernel::task::scheduler::current_group_id());
    kernel::serial::write("\n");
}

void pwd(ShellContext& ctx, int, char*[]) {
    kernel::serial::write(ctx.cwd);
    kernel::serial::write("\n");
}

void cd(ShellContext& ctx, int argc, char* argv[]) {
    if (argc < 2) {
        copy_text(ctx.cwd, "/root", sizeof(ctx.cwd));
        return;
    }
    
    char path[kMaxPath];
    
    if (!arg_to_path(ctx, argv[1], path, "cd <dir>")) {
        return;
    }
    
    kernel::fs::DirEntry probe[1] = {};
    
    if (kernel::fs::g_fs != nullptr && kernel::fs::g_fs->readdir(path, probe, 1) >= 0) {
        copy_text(ctx.cwd, path, sizeof(ctx.cwd));
        return;
    }
    
    kernel::serial::write("cd: directory not found\n");
}

void ld(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }
    
    char path[kMaxPath];
    
    if (argc < 2) {
        copy_text(path, ctx.cwd, sizeof(path));
    
    } else if (!arg_to_path(ctx, argv[1], path, "ld [dir]")) {
        return;
    
    }
    
    kernel::fs::DirEntry entries[32] = {};
    const int n = kernel::fs::g_fs->readdir(path, entries, 32);
    
    if (n < 0) {
        kernel::serial::write("ld: cannot list directory\n");
        return;
    }
    
    for (int i = 0; i < n; ++i) {
        kernel::serial::write(entries[i].is_directory != 0 ? "d " : "f ");
        kernel::serial::write(entries[i].name);
        kernel::serial::write("\n");
    }

    kernel::serial::write("\r\n");

}

void md(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }
    
    char path[kMaxPath];
    
    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "md <dir>")) {
        return;
    }
    
    if (kernel::fs::g_fs->md(path) < 0) {
        kernel::serial::write("md: failed\n");
    }
}

void rd(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }
    
    char path[kMaxPath];
    
    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "rd <dir>")) {
        return;
    }
    
    if (kernel::fs::g_fs->rd(path) < 0) {
        kernel::serial::write("rd: failed (non-empty or missing)\n");
    }
}

void cat(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }
    
    if (argc < 2) {
        kernel::serial::write("cat <file>\n");
        return;
    }
    
    char path[kMaxPath];
    
    if (!arg_to_path(ctx, argv[1], path, "cat <file>")) {
        return;
    }
    
    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);
    
    if (fd < 0) {
        kernel::serial::write("cat: file not found\n");
        return;
    }
    
    char chunk[kFileChunk + 1] = {};
    
    while (true) {
        const int n = kernel::fs::g_fs->read(fd, chunk, kFileChunk);
    
        if (n <= 0) {
            break;
        }
    
        chunk[n] = '\0';
        kernel::serial::write(chunk);
    }
    
    kernel::fs::g_fs->close(fd);
    kernel::serial::write("\n");
}

void touch(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }
    
    char path[kMaxPath];
    
    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "touch <file>")) {
        return;
    }
    
    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate);
    
    if (fd < 0) {
        kernel::serial::write("touch: failed\n");
        return;
    }
    
    kernel::fs::g_fs->close(fd);
}

void rm(ShellContext& ctx, int argc, char* argv[]) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "rm <file>")) {
        return;
    }

    if (kernel::fs::g_fs->unlink(path) < 0) {
        kernel::serial::write("rm: failed\n");
    }
}

void echo(ShellContext&, int argc, char* argv[]) {

    for (int i = 1; i < argc; ++i) {

        if (i > 1) {
            kernel::serial::write(" ");
        }

        kernel::serial::write(argv[i]);
    }

    kernel::serial::write("\n");
}

void halt(ShellContext&, int, char*[]) {
    write_line("powering off");
    machine_poweroff();
}

void exit_shell(ShellContext& ctx, int, char*[]) {
    kernel::serial::write("leaving shell\n");
    ctx.running = false;
}

void reboot(ShellContext&, int, char*[]) {
    write_line("rebooting");
    machine_reboot();
}

void poweroff(ShellContext&, int, char*[]) {
    write_line("powering off");
    machine_poweroff();
}

void history(ShellContext&, int, char*[]) {
    const uint32_t size = history_size();

    if (size == 0) {
        kernel::serial::write("history: empty\n");
        return;
    }

    for (uint32_t i = 0; i < size; ++i) {
        kernel::serial::write("  ");
        write_dec(i + 1u);

        kernel::serial::write(": ");

        const char* entry = history_at(i);

        if (entry != nullptr) {
            kernel::serial::write(entry);
        }

        kernel::serial::write("\n");
    }
}

void cpu(ShellContext&, int argc, char* argv[]) {

    if (argc > 1 && text_equal(argv[1], "raw")) {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

        cpuid(0, 0, eax, ebx, ecx, edx);

        kernel::serial::write("cpuid eax=0x");
        kernel::serial::write_hex(eax);

        kernel::serial::write(" ebx=0x");
        kernel::serial::write_hex(ebx);

        kernel::serial::write(" ecx=0x");
        kernel::serial::write_hex(ecx);

        kernel::serial::write(" edx=0x");
        kernel::serial::write_hex(edx);

        kernel::serial::write("\n");

        return;
    }

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0, 0, eax, ebx, ecx, edx);

    char vendor[13] = {};

    vendor[0] = static_cast<char>(ebx & 0xFFu);
    vendor[1] = static_cast<char>((ebx >> 8) & 0xFFu);
    vendor[2] = static_cast<char>((ebx >> 16) & 0xFFu);
    vendor[3] = static_cast<char>((ebx >> 24) & 0xFFu);
    vendor[4] = static_cast<char>(edx & 0xFFu);
    vendor[5] = static_cast<char>((edx >> 8) & 0xFFu);
    vendor[6] = static_cast<char>((edx >> 16) & 0xFFu);
    vendor[7] = static_cast<char>((edx >> 24) & 0xFFu);
    vendor[8] = static_cast<char>(ecx & 0xFFu);
    vendor[9] = static_cast<char>((ecx >> 8) & 0xFFu);
    vendor[10] = static_cast<char>((ecx >> 16) & 0xFFu);
    vendor[11] = static_cast<char>((ecx >> 24) & 0xFFu);
    vendor[12] = '\0';

    kernel::serial::write("vendor: ");
    kernel::serial::write(vendor);
    kernel::serial::write(" maxleaf=0x");
    kernel::serial::write_hex(eax);
    kernel::serial::write("\n");

    cpuid(1, 0, eax, ebx, ecx, edx);

    const uint32_t stepping = eax & 0xFu;
    const uint32_t model = (eax >> 4) & 0xFu;
    const uint32_t family = (eax >> 8) & 0xFu;
    const uint32_t ext_model = (eax >> 16) & 0xFu;
    const uint32_t ext_family = (eax >> 20) & 0xFFu;
    const uint32_t display_family = (family == 0xFu) ? (family + ext_family) : family;
    const uint32_t display_model = model + ((family == 0x6u || family == 0xFu) ? (ext_model << 4) : 0u);

    kernel::serial::write("signature: family=");
    write_dec(display_family);
    kernel::serial::write(" model=");
    write_dec(display_model);
    kernel::serial::write(" stepping=");
    write_dec(stepping);
    kernel::serial::write(" logical_cpus=");
    write_dec((ebx >> 16) & 0xFFu);
    kernel::serial::write(" apic_id=");
    write_dec((ebx >> 24) & 0xFFu);
    kernel::serial::write("\n");

    kernel::serial::write("family/model/stepping: 0x");
    kernel::serial::write_hex(eax);

    kernel::serial::write(" features edx=0x");
    kernel::serial::write_hex(edx);

    kernel::serial::write(" ecx=0x");
    kernel::serial::write_hex(ecx);

    kernel::serial::write("\n");

    // Try to read extended brand string if available
    uint32_t max_ext = 0;
    cpuid(0x80000000u, 0, max_ext, ebx, ecx, edx);

    if (max_ext >= 0x80000004u) {
        char brand[49] = {};
        uint32_t a = 0, b = 0, c = 0, d = 0;

        cpuid(0x80000002u, 0, a, b, c, d);
        *(uint32_t*)(brand + 0) = a;
        *(uint32_t*)(brand + 4) = b;
        *(uint32_t*)(brand + 8) = c;
        *(uint32_t*)(brand + 12) = d;

        cpuid(0x80000003u, 0, a, b, c, d);
        *(uint32_t*)(brand + 16) = a;
        *(uint32_t*)(brand + 20) = b;
        *(uint32_t*)(brand + 24) = c;
        *(uint32_t*)(brand + 28) = d;

        cpuid(0x80000004u, 0, a, b, c, d);
        *(uint32_t*)(brand + 32) = a;
        *(uint32_t*)(brand + 36) = b;
        *(uint32_t*)(brand + 40) = c;
        *(uint32_t*)(brand + 44) = d;

        brand[48] = '\0';

        kernel::serial::write("model name: ");
        kernel::serial::write(brand);
        kernel::serial::write("\n");
    }
}

void devices(ShellContext&, int, char*[]) {
    kernel::serial::write("devices:\n");
    kernel::serial::write("- serial0 (console I/O)\n");
    kernel::serial::write("- ps/2 keyboard input\n");
    kernel::serial::write("- PCI bus scanner\n");
    kernel::serial::write("- xHCI controller: ");
    kernel::serial::write(kernel::xhci::ready() ? "ready\n" : "unavailable\n");
    kernel::serial::write("- USB mass storage: ");
    kernel::serial::write(kernel::usbms::present() ? "present\n" : "unavailable\n");
    kernel::serial::write("- primary block device: ");
    kernel::serial::write(kernel::block_get_primary() != nullptr ? "registered\n" : "none\n");
    kernel::serial::write("- ramfs rootfs\n");
    kernel::serial::write("- syscall ABI\n");
    kernel::serial::write("- scheduler / tasks\n");
    kernel::serial::write("- gdt / idt / tss\n");
}

void hid(ShellContext&, int, char*[]) {
    kernel::serial::write("hid/input:\n");
    kernel::serial::write("- serial console input\n");
    kernel::serial::write("- ps/2 keyboard polling\n");
    kernel::serial::write("- xHCI controller: ");
    kernel::serial::write(kernel::xhci::ready() ? "ready\n" : "unavailable\n");
    kernel::serial::write("- USB HID: not enumerated yet\n");
    kernel::serial::write("- USB mass storage: ");
    kernel::serial::write(kernel::usbms::present() ? "present\n" : "unavailable\n");
}

void keyboard(ShellContext&, int, char*[]) {
    kernel::serial::write("keyboard:\n");
    kernel::serial::write("- PS/2 scan polling enabled\n");
    kernel::serial::write("- line editing enabled\n");
    kernel::serial::write("- console backend: serial + VGA\n");
}

void drivers(ShellContext&, int, char*[]) {
    kernel::serial::write("drivers:\n");
    kernel::serial::write("- serial\n");
    kernel::serial::write("- gdt/tss\n");
    kernel::serial::write("- idt/interrupts\n");
    kernel::serial::write("- pic\n");
    kernel::serial::write("- pit\n");
    kernel::serial::write("- pmm\n");
    kernel::serial::write("- vmm\n");
    kernel::serial::write("- heap\n");
    kernel::serial::write("- ramfs\n");
    kernel::serial::write("- syscall\n");
    kernel::serial::write("- scheduler\n");
}

void pci(ShellContext&, int, char*[]) {
    kernel::pci::Device devs[64];
    uint32_t found = 0;

    if (!kernel::pci::scan_devices(devs, 64, &found) || found == 0) {
        kernel::serial::write("pci: no devices found\n");
        return;
    }

    for (uint32_t i = 0; i < found; ++i) {
        const kernel::pci::Device& d = devs[i];

        kernel::serial::write("PCI ");
        write_dec(d.bus);
        kernel::serial::write(":");
        write_dec(d.slot);
        kernel::serial::write(".");
        write_dec(d.func);

        kernel::serial::write(" ");
        const char* vendor_name = pci_vendor_name(d.vendor_id);

        if (vendor_name != nullptr) {
            kernel::serial::write(vendor_name);
            kernel::serial::write(" ");
        }

        kernel::serial::write("vendor=0x");
        kernel::serial::write_hex(d.vendor_id);
        kernel::serial::write(" device=0x");
        kernel::serial::write_hex(d.device_id);

        kernel::serial::write(" class=");
        kernel::serial::write(pci_class_name(d.class_code, d.subclass, d.prog_if));
        kernel::serial::write(" [0x");
        kernel::serial::write_hex(d.class_code);
        kernel::serial::write("/0x");
        kernel::serial::write_hex(d.subclass);
        kernel::serial::write("/0x");
        kernel::serial::write_hex(d.prog_if);
        kernel::serial::write("]");

        kernel::serial::write(" irq=");
        write_dec(d.irq);

        for (int b = 0; b < 6; ++b) {
            const uint32_t raw = d.bar[b];

            if (raw == 0u) {
                continue;
            }

            kernel::serial::write("  BAR");
            write_dec(static_cast<uint32_t>(b));

            if ((raw & 1u) != 0u) {
                kernel::serial::write(" io=0x");
                kernel::serial::write_hex(raw & ~0x3u);
                continue;
            }

            const bool is_64bit = ((raw & 0x6u) == 0x4u) && (b + 1 < 6);

            if (is_64bit) {
                const uint64_t addr = (static_cast<uint64_t>(d.bar[b + 1]) << 32) | static_cast<uint64_t>(raw & ~0xFu);
                kernel::serial::write(" mmio64=");
                kernel::serial::write_hex64(addr);
                ++b;
            } else {
                kernel::serial::write(" mmio=0x");
                kernel::serial::write_hex(raw & ~0xFu);
            }
        }

        kernel::serial::write("\n");
    }
}

void ahci(ShellContext&, int, char*[]) {
    kernel::ahci::print_info();
}

void cp(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    if (argc < 3) {
        kernel::serial::write("cp <src> <dst>\n");
        return;
    }

    char src[kMaxPath];
    char dst[kMaxPath];

    if (!arg_to_path(ctx, argv[1], src, "cp <src> <dst>") ||
        !arg_to_path(ctx, argv[2], dst, "cp <src> <dst>")) {
        return;
    }

    const int src_fd = kernel::fs::g_fs->open(src, kernel::fs::kOpenRead);

    if (src_fd < 0) {
        kernel::serial::write("cp: source not found\n");
        return;
    }

    const int dst_fd = 
        kernel::fs::g_fs->open(dst, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate | kernel::fs::kOpenTruncate);
    
    if (dst_fd < 0) {
        kernel::fs::g_fs->close(src_fd);
        kernel::serial::write("cp: cannot open destination\n");
        return;
    }

    char buf[kFileChunk];

    while (true) {
        const int n = kernel::fs::g_fs->read(src_fd, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }

        if (kernel::fs::g_fs->write(dst_fd, buf, static_cast<uint32_t>(n)) < 0) {
            kernel::serial::write("cp: write error\n");
            break;
        }
    }

    kernel::fs::g_fs->close(src_fd);
    kernel::fs::g_fs->close(dst_fd);
}

void mv(ShellContext& ctx, int argc, char* argv[]) {

    if (argc < 3) {
        kernel::serial::write("mv <src> <dst>\n");
        return;
    }

    cp(ctx, argc, argv);
    char src[kMaxPath];

    if (!arg_to_path(ctx, argv[1], src, "mv <src> <dst>")) {
        return;
    }

    if (kernel::fs::g_fs == nullptr || kernel::fs::g_fs->unlink(src) < 0) {
        kernel::serial::write("mv: cleanup failed\n");
    }
}

void head(ShellContext& ctx, int argc, char* argv[]) {
    if (argc < 2) {
        kernel::serial::write("head <file> [lines]\n");
        return;
    }

    char path[kMaxPath];

    if (!arg_to_path(ctx, argv[1], path, "head <file> [lines]")) {
        return;
    }

    uint32_t lines = 10;

    if (argc >= 3 && !parse_u32(argv[2], &lines)) {
        kernel::serial::write("head: invalid line count\n");
        return;
    }

    print_path_lines(path, lines, false);

    kernel::serial::write("\n");
}

void tail(ShellContext& ctx, int argc, char* argv[]) {
    if (argc < 2) {
        kernel::serial::write("tail <file> [lines]\n");
        return;
    }

    char path[kMaxPath];
    if (!arg_to_path(ctx, argv[1], path, "tail <file> [lines]")) {
        return;
    }

    uint32_t lines = 10;

    if (argc >= 3 && !parse_u32(argv[2], &lines)) {
        kernel::serial::write("tail: invalid line count\n");
        return;
    }

    print_path_lines(path, lines, true);
    kernel::serial::write("\n");
}

void wc(ShellContext& ctx, int argc, char* argv[]) {

    if (argc < 2) {
        kernel::serial::write("wc <file>\n");
        return;
    }

    char path[kMaxPath];

    if (!arg_to_path(ctx, argv[1], path, "wc <file>")) {
        return;
    }

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        kernel::serial::write("wc: file not found\n");
        return;
    }

    uint32_t lines = 0;
    uint32_t words = 0;
    uint32_t bytes = 0;

    bool in_word = false;
    char buf[kFileChunk];

    while (true) {
        const int n = kernel::fs::g_fs->read(fd, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }

        bytes += static_cast<uint32_t>(n);

        for (int i = 0; i < n; ++i) {
            const char c = buf[i];

            if (c == '\n') {
                ++lines;
            }

            if (is_space(c) || c == '\n' || c == '\r') {
                in_word = false;

            } else if (!in_word) {
                in_word = true;
                ++words;
            }
        }
    }

    kernel::fs::g_fs->close(fd);

    kernel::serial::write("lines=");
    write_dec(lines);

    kernel::serial::write(" words=");
    write_dec(words);

    kernel::serial::write(" bytes=");
    write_dec(bytes);

    kernel::serial::write("\n");
}

void cmp(ShellContext& ctx, int argc, char* argv[]) {
    if (argc < 3) {
        kernel::serial::write("cmp <file1> <file2>\n");
        return;
    }
    
    char a_path[kMaxPath];
    char b_path[kMaxPath];
    
    if (!arg_to_path(ctx, argv[1], a_path, "cmp <file1> <file2>") ||
        !arg_to_path(ctx, argv[2], b_path, "cmp <file1> <file2>")) {
        return;
    }
    
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }
    
    const int a_fd = kernel::fs::g_fs->open(a_path, kernel::fs::kOpenRead);
    const int b_fd = kernel::fs::g_fs->open(b_path, kernel::fs::kOpenRead);
    
    if (a_fd < 0 || b_fd < 0) {
        if (a_fd >= 0) kernel::fs::g_fs->close(a_fd);
        if (b_fd >= 0) kernel::fs::g_fs->close(b_fd);
    
        kernel::serial::write("cmp: file missing\n");
    
        return;
    }
    
    bool same = true;
    char a_buf[kFileChunk];
    char b_buf[kFileChunk];
    
    while (true) {
        const int an = kernel::fs::g_fs->read(a_fd, a_buf, sizeof(a_buf));
        const int bn = kernel::fs::g_fs->read(b_fd, b_buf, sizeof(b_buf));

        if (an != bn) {
            same = false;
            break;
        }

        if (an <= 0) {
            break;
        }

        for (int i = 0; i < an; ++i) {

            if (a_buf[i] != b_buf[i]) {
                same = false;
                break;
            }
        }

        if (!same) {
            break;
        }
    }

    kernel::fs::g_fs->close(a_fd);
    kernel::fs::g_fs->close(b_fd);
    kernel::serial::write(same ? "cmp: equal\n" : "cmp: different\n");
}

void stat(ShellContext& ctx, int argc, char* argv[]) {
    if (argc < 2) {
        kernel::serial::write("stat <path>\n");
        return;
    }

    char path[kMaxPath];

    if (!arg_to_path(ctx, argv[1], path, "stat <path>")) {
        return;
    }

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");

        return;
    }

    kernel::fs::DirEntry probe[16] = {};
    const int dn = kernel::fs::g_fs->readdir(path, probe, 16);

    if (dn >= 0) {
        kernel::serial::write("type=dir entries=");
        write_dec(static_cast<uint32_t>(dn));

        kernel::serial::write(" path=");
        kernel::serial::write(path);

        kernel::serial::write("\n");

        return;
    }

    uint32_t size = 0;

    if (get_file_size(path, &size)) {
        kernel::serial::write("type=file size=");
        write_dec(size);

        kernel::serial::write(" path=");
        kernel::serial::write(path);

        kernel::serial::write("\n");

        return;
    }
    kernel::serial::write("stat: not found\n");
}

void tree_walk(const char* path, uint32_t depth) {
    if (kernel::fs::g_fs == nullptr || depth > kMaxTreeDepth) {
        return;
    }

    kernel::fs::DirEntry entries[16] = {};

    const int n = kernel::fs::g_fs->readdir(path, entries, 16);

    if (n < 0) {
        return;
    }

    for (int i = 0; i < n; ++i) {

        for (uint32_t d = 0; d < depth; ++d) {
            kernel::serial::write("  ");

        }

        kernel::serial::write(entries[i].is_directory != 0 ? "+ " : "- ");
        kernel::serial::write(entries[i].name);
        kernel::serial::write("\n");

        if (entries[i].is_directory != 0) {
            char child[kMaxPath];

            if (text_equal(path, "/")) {
                child[0] = '/';
                copy_text(child + 1, entries[i].name, sizeof(child) - 1);

            } else {
                copy_text(child, path, sizeof(child));
                const uint32_t len = text_len(child);

                if (len + 1 < sizeof(child)) {
                    child[len] = '/';
                    child[len + 1] = '\0';

                    copy_text(child + len + 1, entries[i].name, sizeof(child) - len - 1);

                }
            }

            tree_walk(child, depth + 1);
        }
    }
}

void tree(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2) {
        copy_text(path, ctx.cwd, sizeof(path));

    } else if (!arg_to_path(ctx, argv[1], path, "tree [dir]")) {
        return;

    }

    kernel::serial::write(path);
    kernel::serial::write("\n");
    tree_walk(path, 1);
}

void env(ShellContext& ctx, int, char*[]) {
    kernel::serial::write("USER=root\n");
    kernel::serial::write("HOME=/root\n");
    kernel::serial::write("PWD=");
    kernel::serial::write(ctx.cwd);

    kernel::serial::write("\n");

    kernel::serial::write("TICKS=");
    write_dec(kernel::arch::x86::interrupts::ticks());

    kernel::serial::write("\n");

}

bool uname_flag_present(int argc, char* argv[], char flag) {

    for (int i = 1; i < argc; ++i) {

        if (argv[i] == nullptr || argv[i][0] != '-') {
            continue;
        }

        for (uint32_t j = 1; argv[i][j] != '\0'; ++j) {

            if (argv[i][j] == flag) {
                return true;
            }
        }
    }

    return false;
}

void uname_print_field(const char* key, const char* value, bool& first) {
    if (!first) {
        kernel::serial::write(" ");
    }

    kernel::serial::write(value);
    first = false;

    (void)key;
}

void uname(ShellContext&, int argc, char* argv[]) {
    const bool all = (argc <= 1) || uname_flag_present(argc, argv, 'a');
    const bool show_sysname = all || uname_flag_present(argc, argv, 's');
    const bool show_nodename = all || uname_flag_present(argc, argv, 'n');
    const bool show_release = all || uname_flag_present(argc, argv, 'r');
    const bool show_version = all || uname_flag_present(argc, argv, 'v');
    const bool show_machine = all || uname_flag_present(argc, argv, 'm');
    const bool show_processor = all || uname_flag_present(argc, argv, 'p');
    const bool show_hardware = all || uname_flag_present(argc, argv, 'i');

    bool first = true;

    if (show_sysname) {
        uname_print_field("sysname", kKernelName, first);
    }

    if (show_nodename) {
        uname_print_field("nodename", kKernelNodeName, first);
    }

    if (show_release) {
        uname_print_field("release", kKernelRelease, first);
    }

    if (show_version) {
        uname_print_field("version", kKernelVersion, first);
    }

    if (show_machine) {
        uname_print_field("machine", kKernelMachine, first);
    }

    if (show_processor) {
        uname_print_field("processor", kKernelProcessor, first);
    }

    if (show_hardware) {
        uname_print_field("hardware", kKernelHardware, first);
    }

    if (first) {
        kernel::serial::write(kKernelName);

    } else if (!all) {
        kernel::serial::write(" ");
        kernel::serial::write(kKernelBuildStamp);
    }

    if (all) {
        kernel::serial::write(" ");
        kernel::serial::write(kKernelBuildStamp);
    }

    kernel::serial::write("\n");
}

void ctrue(ShellContext&, int, char*[]) {
    kernel::serial::write("true\n");
}

void cfalse(ShellContext&, int, char*[]) {
    kernel::serial::write("false\n");
}

void basename(ShellContext& ctx, int argc, char* argv[]) {
    const char* in = (argc >= 2) ? argv[1] : ctx.cwd;
    char path[kMaxPath];

    if (in[0] == '/') {
        copy_text(path, in, sizeof(path));

    } else if (!resolve_path(ctx.cwd, in, path, sizeof(path))) {

        kernel::serial::write("basename: invalid path\n");
        return;
    }

    kernel::serial::write(path_basename(path));
    kernel::serial::write("\n");
}

void dirname(ShellContext& ctx, int argc, char* argv[]) {
    const char* in = (argc >= 2) ? argv[1] : ctx.cwd;

    char path[kMaxPath];

    if (in[0] == '/') {
        copy_text(path, in, sizeof(path));

    } else if (!resolve_path(ctx.cwd, in, path, sizeof(path))) {
        kernel::serial::write("dirname: invalid path\n");
        return;
    }

    char out[kMaxPath];

    path_dirname(path, out, sizeof(out));

    kernel::serial::write(out);
    kernel::serial::write("\n");
}

void exists(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "exists <path>")) {
        return;
    }

    kernel::serial::write(path_exists(path) ? "yes\n" : "no\n");
}

void isfile(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "isfile <path>")) {
        return;
    }

    kernel::serial::write(is_regular_file(path) ? "yes\n" : "no\n");
}

void isdir(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "isdir <path>")) {
        return;
    }

    kernel::serial::write(is_directory(path) ? "yes\n" : "no\n");
}

void size(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "size <file>")) {
        return;
    }

    uint32_t size = 0;

    if (!get_file_size(path, &size)) {
        kernel::serial::write("size: not a file\n");
        return;
    }

    write_dec(size);

    kernel::serial::write("\n");
}

void write(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 3 || !arg_to_path(ctx, argv[1], path, "write <file> <text...>")) {
        return;
    }

    char text[kMaxLine];

    join_rest_args(argc, argv, 2, text, sizeof(text));

    if (!write_file_text(path, text, false)) {
        kernel::serial::write("write: failed\n");
    }
}

void append(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 3 || !arg_to_path(ctx, argv[1], path, "append <file> <text...>")) {
        return;
    }

    char text[kMaxLine];

    join_rest_args(argc, argv, 2, text, sizeof(text));

    if (!write_file_text(path, text, true)) {
        kernel::serial::write("append: failed\n");
    }
}

void truncate(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "truncate <file>")) {
        return;
    }

    if (!write_file_text(path, "", false)) {
        kernel::serial::write("truncate: failed\n");
    }
}

void nl(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "nl <file>")) {
        return;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        kernel::serial::write("nl: file not found\n");
        return;
    }

    char buf[kFileChunk];
    uint32_t line = 1;

    write_dec(line);
    kernel::serial::write(": ");

    while (true) {
        const int n = kernel::fs::g_fs->read(fd, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }

        for (int i = 0; i < n; ++i) {
            kernel::serial::write_char(buf[i]);

            if (buf[i] == '\n') {
                ++line;
                write_dec(line);
                kernel::serial::write(": ");
            }
        }
    }

    kernel::fs::g_fs->close(fd);
    kernel::serial::write("\n");
}

void hexdump(ShellContext& ctx, int argc, char* argv[]) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    char path[kMaxPath];

    if (argc < 2 || !arg_to_path(ctx, argv[1], path, "hexdump <file>")) {
        return;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        kernel::serial::write("hexdump: file not found\n");
        return;
    }

    uint8_t buf[16];
    uint32_t offset = 0;

    while (true) {
        const int n = kernel::fs::g_fs->read(fd, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }

        kernel::serial::write_hex(offset);
        kernel::serial::write(": ");

        for (int i = 0; i < n; ++i) {
            const uint8_t b = buf[i];
            const char* hex = "0123456789ABCDEF";

            kernel::serial::write_char(hex[(b >> 4) & 0xF]);
            kernel::serial::write_char(hex[b & 0xF]);
            kernel::serial::write_char(' ');
        }

        kernel::serial::write("\n");
        offset += static_cast<uint32_t>(n);
    }

    kernel::fs::g_fs->close(fd);
}

bool text_contains(const char* s, const char* needle) {

    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }

    if (s == nullptr) {
        return false;
    }

    for (uint32_t i = 0; s[i] != '\0'; ++i) {
        uint32_t j = 0;

        while (needle[j] != '\0' && s[i + j] == needle[j]) {
            ++j;
        }

        if (needle[j] == '\0') {
            return true;
        }
    }

    return false;
}

void find_walk(const char* path, const char* needle, uint32_t depth) {
    if (kernel::fs::g_fs == nullptr || depth > kMaxTreeDepth) {
        return;
    }

    kernel::fs::DirEntry entries[16] = {};

    const int n = kernel::fs::g_fs->readdir(path, entries, 16);

    if (n < 0) {
        return;
    }

    for (int i = 0; i < n; ++i) {
        char child[kMaxPath];

        if (text_equal(path, "/")) {
            child[0] = '/';
            copy_text(child + 1, entries[i].name, sizeof(child) - 1);

        } else {
            copy_text(child, path, sizeof(child));
            const uint32_t len = text_len(child);

            if (len + 1 >= sizeof(child)) {
                continue;
            }

            child[len] = '/';
            child[len + 1] = '\0';
            copy_text(child + len + 1, entries[i].name, sizeof(child) - len - 1);
        }

        if (text_contains(entries[i].name, needle)) {
            kernel::serial::write(child);
            kernel::serial::write("\n");
        }

        if (entries[i].is_directory != 0) {
            find_walk(child, needle, depth + 1);
        }
    }
}

void find(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];
    const char* needle = "";

    if (argc < 2) {
        copy_text(path, ctx.cwd, sizeof(path));

    } else if (!arg_to_path(ctx, argv[1], path, "find [dir] [name-substring]")) {

        return;
    }

    if (argc >= 3) {
        needle = argv[2];
    }

    find_walk(path, needle, 0);
}

void grep(ShellContext& ctx, int argc, char* argv[]) {
    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    if (argc < 3) {
        kernel::serial::write("grep <text> <file>\n");
        return;
    }
    const char* needle = argv[1];
    char path[kMaxPath];

    if (!arg_to_path(ctx, argv[2], path, "grep <text> <file>")) {
        return;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        kernel::serial::write("grep: file not found\n");
        return;
    }
    char buf[1024];
    uint32_t n_total = 0;

    while (n_total < sizeof(buf) - 1) {
        const int n = kernel::fs::g_fs->read(fd, &buf[n_total], sizeof(buf) - 1 - n_total);

        if (n <= 0) {
            break;
        }

        n_total += static_cast<uint32_t>(n);
    }

    kernel::fs::g_fs->close(fd);
    buf[n_total] = '\0';

    if (text_contains(buf, needle)) {
        kernel::serial::write("match\n");

    } else {
        kernel::serial::write("no-match\n");
    }
}

void sleep(ShellContext&, int argc, char* argv[]) {
    uint32_t ticks = 10;

    if (argc >= 2 && !parse_u32(argv[1], &ticks)) {
        kernel::serial::write("sleep: invalid ticks\n");
        return;
    }

    kernel::task::scheduler::sleep_current(ticks, kernel::arch::x86::interrupts::ticks());

    while (kernel::task::scheduler::is_current_sleeping(kernel::arch::x86::interrupts::ticks())) {
        asm volatile("hlt");
    }
}

void tick(ShellContext&, int, char*[]) {
    write_dec(kernel::arch::x86::interrupts::ticks());
    kernel::serial::write("\n");
}

void uptime(ShellContext&, int, char*[]) {
    const uint32_t ticks = kernel::arch::x86::interrupts::ticks();
    kernel::serial::write("ticks=");
    write_dec(ticks);

    kernel::serial::write(" sec=");
    write_dec(ticks / 100);

    kernel::serial::write("\n");
}

void ps(ShellContext&, int, char*[]) {
    kernel::serial::write("pid=");
    write_dec(kernel::task::scheduler::current_process_id());

    kernel::serial::write(" tid=");
    write_dec(kernel::task::scheduler::current_thread_id());

    kernel::serial::write(" ring=");
    write_dec(kernel::task::scheduler::current_ring_level());

    kernel::serial::write(" proc_count=");
    write_dec(kernel::task::scheduler::process_count());

    kernel::serial::write(" thread_count=");
    write_dec(kernel::task::scheduler::thread_count());

    kernel::serial::write("\n");
}

void meminfo(ShellContext&, int, char*[]) {
    kernel::serial::write("heap_used=");
    write_dec(kernel::mm::heap::used_bytes());

    kernel::serial::write(" heap_mapped=");
    write_dec(kernel::mm::heap::mapped_bytes());

    kernel::serial::write(" pmm_total_frames=");
    write_dec(kernel::mm::pmm::total_frames());

    kernel::serial::write(" pmm_free_frames=");
    write_dec(kernel::mm::pmm::free_frames());

    kernel::serial::write("\n");
}

void free(ShellContext&, int, char*[]) {
    constexpr uint32_t kPageSize = 4096u;
    const uint32_t total_frames = kernel::mm::pmm::total_frames();
    const uint32_t free_frames = kernel::mm::pmm::free_frames();
    const uint32_t used_frames = (total_frames >= free_frames) ? (total_frames - free_frames) : 0u;
    const uint32_t total_kib = total_frames * 4u;
    const uint32_t used_kib = used_frames * 4u;
    const uint32_t free_kib = free_frames * 4u;

    kernel::serial::write("              total        used        free\n");
    kernel::serial::write("Mem:     ");
    write_dec(total_kib);

    kernel::serial::write(" KiB   ");
    write_dec(used_kib);

    kernel::serial::write(" KiB   ");
    write_dec(free_kib);

    kernel::serial::write(" KiB\n");

    kernel::serial::write("Heap:    ");
    write_dec(kernel::mm::heap::mapped_bytes() / 1024u);

    kernel::serial::write(" KiB mapped, ");
    write_dec(kernel::mm::heap::used_bytes() / 1024u);

    kernel::serial::write(" KiB used\n");

    kernel::serial::write("Frames:  total=");
    write_dec(total_frames);

    kernel::serial::write(" free=");
    write_dec(free_frames);

    kernel::serial::write(" used=");
    write_dec(used_frames);

    kernel::serial::write(" page_size=");
    write_dec(kPageSize);

    kernel::serial::write("\n");
}

void df(ShellContext&, int, char*[]) {
    kernel::serial::write("storage_ready=");
    kernel::serial::write(kernel::storage_ready() ? "yes" : "no");
    kernel::serial::write(" data_start_lba=");
    write_dec(kernel::storage_data_start_lba());

    kernel::serial::write(" next_free_lba=");
    write_dec(kernel::storage_next_free_lba());

    kernel::serial::write(" used_sectors=");
    write_dec(kernel::storage_used_sectors());

    kernel::serial::write(" approx_bytes=");
    write_dec(kernel::storage_used_sectors() * 512u);

    kernel::serial::write("\n");
}

void lsblk(ShellContext&, int, char*[]) {
    kernel::serial::write("NAME\tSIZE\tBLOCK\tTYPE\tSOURCE\n");

    bool any = false;

    kernel::block_visit_devices([](kernel::BlockDevice* dev, void* user_data) {
        bool* any = reinterpret_cast<bool*>(user_data);

        if (dev == nullptr) {
            return;
        }

        *any = true;

        kernel::serial::write(dev->name != nullptr ? dev->name : "unnamed");
        kernel::serial::write("\t");

        if (dev->block_count == 0u || dev->block_size == 0u) {
            kernel::serial::write("unknown\t");

        } else {
            const uint64_t bytes = dev->block_count * static_cast<uint64_t>(dev->block_size);
            const uint64_t mib = bytes / (1024ull * 1024ull);

            if (mib > 0u) {
                write_dec(static_cast<uint32_t>(mib));
                kernel::serial::write(" MiB\t");

            } else {
                const uint64_t kib = bytes / 1024ull;
                write_dec(static_cast<uint32_t>(kib));
                kernel::serial::write(" KiB\t");
            }
        }

        write_dec(dev->block_size);
        kernel::serial::write("\t disk\tdevice\n");
    }, &any);

    if (!any) {
        kernel::serial::write("lsdisk: no block device registered\n");
    }
}

void sync(ShellContext&, int, char*[]) {
    kernel::serial::write("sync: storage writes are immediate on this build\n");
}

void pathjoin(ShellContext& ctx, int argc, char* argv[]) {

    if (argc < 3) {
        kernel::serial::write("pathjoin <a> <b>\n");
        return;
    }

    char joined[kMaxPath];

    if (!resolve_path(argv[1][0] == '/' ? "/" : ctx.cwd, argv[1], joined, sizeof(joined))) {
        kernel::serial::write("pathjoin: invalid\n");
        return;
    }

    if (!resolve_path(joined, argv[2], joined, sizeof(joined))) {
        kernel::serial::write("pathjoin: invalid\n");
        return;
    }

    kernel::serial::write(joined);
    kernel::serial::write("\n");
}

void realpath(ShellContext& ctx, int argc, char* argv[]) {
    char path[kMaxPath];

    if (argc < 2) {
        copy_text(path, ctx.cwd, sizeof(path));

    } else if (!arg_to_path(ctx, argv[1], path, "realpath <path>")) {
        return;
    }

    kernel::serial::write(path);
    kernel::serial::write("\n");
}

void seq(ShellContext&, int argc, char* argv[]) {
    uint32_t start = 1;
    uint32_t end = 0;
    uint32_t step = 1;

    if (argc == 2) {

        if (!parse_u32(argv[1], &end)) {
            kernel::serial::write("seq <end> | seq <start> <end> [step]\n");
            return;
        }

    } else if (argc >= 3) {

        if (!parse_u32(argv[1], &start) || !parse_u32(argv[2], &end)) {
            kernel::serial::write("seq <end> | seq <start> <end> [step]\n");
            return;
        }

        if (argc >= 4 && !parse_u32(argv[3], &step)) {
            kernel::serial::write("seq: invalid step\n");
            return;
        }

    } else {
        kernel::serial::write("seq <end> | seq <start> <end> [step]\n");
        return;
    }

    if (step == 0 || start > end) {
        kernel::serial::write("seq: invalid range\n");
        return;
    }

    for (uint32_t v = start; v <= end; v += step) {

        write_dec(v);

        kernel::serial::write("\n");

        if (end - v < step) {
            break;
        }
    }
}

void repeat(ShellContext&, int argc, char* argv[]) {

    if (argc < 3) {
        kernel::serial::write("repeat <n> <text...>\n");
        return;
    }

    uint32_t n = 0;

    if (!parse_u32(argv[1], &n)) {
        kernel::serial::write("repeat: invalid n\n");
        return;
    }

    char text[kMaxLine];

    join_rest_args(argc, argv, 2, text, sizeof(text));

    for (uint32_t i = 0; i < n; ++i) {
        kernel::serial::write(text);
        kernel::serial::write("\n");
    }
}

void catb(ShellContext& ctx, int argc, char* argv[]) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    if (argc < 2) {
        kernel::serial::write("catb <file>\n");
        return;

    }

    char path[kMaxPath];

    if (!arg_to_path(ctx, argv[1], path, "catb <file>")) {
        return;
    }

    const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenRead);

    if (fd < 0) {
        kernel::serial::write("catb: file not found\n");
        return;
    }

    char chunk[kFileChunk];

    while (true) {

        const int n = kernel::fs::g_fs->read(fd, chunk, sizeof(chunk));

        if (n <= 0) {
            break;
        }

        for (int i = 0; i < n; ++i) {
            kernel::serial::write_char(chunk[i]);
        }
    }

    kernel::fs::g_fs->close(fd);
}

void countdir(ShellContext& ctx, int argc, char* argv[]) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    char path[kMaxPath];

    if (argc < 2) {
        copy_text(path, ctx.cwd, sizeof(path));

    } else if (!arg_to_path(ctx, argv[1], path, "countdir [dir]")) {
        return;

    }

    kernel::fs::DirEntry entries[32] = {};
    const int n = kernel::fs::g_fs->readdir(path, entries, 32);

    if (n < 0) {
        kernel::serial::write("countdir: failed\n");
        return;
    }

    write_dec(static_cast<uint32_t>(n));

    kernel::serial::write("\n");
}

void touchmany(ShellContext& ctx, int argc, char* argv[]) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    if (argc < 2) {
        kernel::serial::write("touchmany <file1> [file2 ...]\n");
        return;
    }

    for (int i = 1; i < argc; ++i) {
        char path[kMaxPath];

        if (!arg_to_path(ctx, argv[i], path, "touchmany <file1> [file2 ...]")) {
            return;
        }

        const int fd = kernel::fs::g_fs->open(path, kernel::fs::kOpenWrite | kernel::fs::kOpenCreate);

        if (fd < 0) {
            kernel::serial::write("touchmany: failed for ");
            kernel::serial::write(argv[i]);

            kernel::serial::write("\n");

            continue;
        }

        kernel::fs::g_fs->close(fd);
    }
}

void rmmany(ShellContext& ctx, int argc, char* argv[]) {

    if (kernel::fs::g_fs == nullptr) {
        kernel::serial::write("fs unavailable\n");
        return;
    }

    if (argc < 2) {
        kernel::serial::write("rmmany <file1> [file2 ...]\n");
        return;
    }

    for (int i = 1; i < argc; ++i) {
        char path[kMaxPath];
        if (!arg_to_path(ctx, argv[i], path, "rmmany <file1> [file2 ...]")) {

            return;
        }

        if (kernel::fs::g_fs->unlink(path) < 0) {
            kernel::serial::write("rmmany: failed for ");
            kernel::serial::write(argv[i]);
            kernel::serial::write("\n");
        }
    }
}

}  // namespace

namespace kernel::shell {

void run() {

    ShellContext ctx{};
    copy_text(ctx.cwd, "/root", sizeof(ctx.cwd));
    ctx.running = true;

    char line[kMaxLine];

    while (ctx.running) {

        print_prompt(ctx);

        uint32_t pos = 0;
        line[0] = '\0';

        while (true) {

            const char c = kernel::serial::read_char_blocking();

            if (c == '\r' || c == '\n') {
                kernel::serial::write("\n");
                break;
            }

            if (c == '\x1b') {
                const char seq1 = kernel::serial::read_char_blocking();
                const char seq2 = kernel::serial::read_char_blocking();

                if (seq1 == '[' && seq2 == 'A') {
                    history_move_up(ctx, line, pos);

                } else if (seq1 == '[' && seq2 == 'B') {
                    history_move_down(ctx, line, pos);
                }

                continue;
            }

            if (c == '\t') {
                complete_command_line(ctx, line, pos);
                continue;
            }

            if (c == 0x08 || c == 0x7F) {

                if (pos > 0) {
                    --pos;
                    kernel::serial::write("\b \b");
                }

                continue;
            }

            if (pos + 1 < sizeof(line)) {
                line[pos++] = c;
                kernel::serial::write_char(c);
            }
        }

        line[pos] = '\0';

        if (pos == 0) {
            history_reset_view();
            continue;
        }

        history_append(line);
        history_reset_view();

        char* argv[kMaxArgs] = {};
        const int argc = parse_args(line, argv);

        if (argc <= 0) {
            continue;
        }

        const CommandEntry* command = find_command(argv[0]);

        if (command == nullptr) {
            kernel::serial::write(kAnsiRed);

            kernel::serial::write("unknown command: ");
            kernel::serial::write(argv[0]);

            kernel::serial::write(kAnsiReset);
            
            kernel::serial::write("\n");


            const char* suggestion = suggest_command(argv[0]);
            
            if (suggestion != nullptr) {
                kernel::serial::write(kAnsiGreen);
            
                kernel::serial::write("do you mean: ");
                kernel::serial::write(suggestion);
            
                kernel::serial::write("?\n");
            
                kernel::serial::write(kAnsiReset);
            }
            
            continue;
        }
        command->handler(ctx, argc, argv);
    }
}

}  // namespace kernel::shell