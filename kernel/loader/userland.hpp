#pragma once

#include <stdint.h>
#include "elf.hpp"
#include "../mm/pmm.hpp"
#include "../mm/vmm.hpp"
#include "../ring3/entry.hpp"

namespace kernigham::loader {

struct UserlandImage {
    const uint8_t* data;
    uint32_t size;
};

class UserBinaryLoader {
public:
    static bool load(const UserlandImage& img, const char* name) {
        if (img.size < sizeof(ELFHeader)) {
            debug_log("[loader] image too small");
            return false;
        }

        const ELFHeader* hdr = reinterpret_cast<const ELFHeader*>(img.data);
        if (!is_elf(hdr)) {
            debug_log("[loader] invalid ELF magic");
            return false;
        }

        if (hdr->e_machine != EM_386) {
            debug_log("[loader] unsupported machine type");
            return false;
        }
        if (hdr->e_phentsize != sizeof(ProgramHeader)) {
            debug_log("[loader] invalid ph entry size");
            return false;
        }
        if (hdr->e_phnum == 0) {
            debug_log("[loader] no loadable segments");
            return false;
        }

        debug_log_fmt("[loader] ELF entry=0x%X phnum=%u",
                      hdr->e_entry, hdr->e_phnum);

        uint32_t phoff = hdr->e_phoff;
        uint32_t phnum = hdr->e_phnum;
        uint32_t phentsize = hdr->e_phentsize;

        if (phoff + phnum * phentsize > img.size) {
            debug_log("[loader] program headers out of bounds");
            return false;
        }

        uint32_t highest_addr = 0;
        for (uint32_t i = 0; i < phnum; i++) {
            const auto* ph = reinterpret_cast<const ProgramHeader*>(
                img.data + phoff + i * phentsize
            );

            if (ph->p_type != PT_LOAD) continue;
            if (ph->p_memsz < ph->p_filesz) {
                debug_log("[loader] memsz < filesz");
                return false;
            }

            uint32_t vaddr = ph->p_vaddr;
            uint32_t filesz = ph->p_filesz;
            uint32_t memsz = ph->p_memsz;
            uint32_t offset = ph->p_offset;
            uint32_t seg_end = vaddr + memsz;
            if (seg_end > highest_addr) {
                highest_addr = seg_end;
            }

            debug_log_fmt("[loader] segment: vaddr=0x%X filesz=%u",
                          vaddr, filesz);

            if (offset + filesz > img.size) {
                debug_log("[loader] segment data out of bounds");
                return false;
            }

            const uint32_t page_start = vaddr & 0xFFFFF000u;
            const uint32_t page_end = (seg_end + 0xFFFu) & 0xFFFFF000u;
            for (uint32_t page = page_start; page < page_end; page += 0x1000u) {
                const uint32_t phys = kernel::mm::pmm::alloc_frame();
                if (phys == 0) {
                    debug_log("[loader] out of physical frames");
                    return false;
                }
                const bool writable = (ph->p_flags & PF_W) != 0;
                if (!kernel::mm::vmm::map_page(page, phys, writable, true)) {
                    debug_log("[loader] map user segment failed");
                    return false;
                }
            }

            copy_bytes(
                reinterpret_cast<uint8_t*>(vaddr),
                img.data + offset,
                filesz);
            if (memsz > filesz) {
                zero_bytes(reinterpret_cast<uint8_t*>(vaddr + filesz), memsz - filesz);
            }
        }

        if (highest_addr == 0) {
            debug_log("[loader] no PT_LOAD segment");
            return false;
        }

        const uint32_t stack_base = (highest_addr + 0x1FFFFu) & 0xFFFFF000u;
        const uint32_t stack_page = stack_base;
        const uint32_t stack_phys = kernel::mm::pmm::alloc_frame();
        if (stack_phys == 0 ||
            !kernel::mm::vmm::map_page(stack_page, stack_phys, true, true)) {
            debug_log("[loader] map user stack failed");
            return false;
        }
        const uint32_t user_stack_top = stack_page + 0x1000u - 16u;
        debug_log_fmt("[loader] userland %s loaded entry=0x%X", name, hdr->e_entry);
        kernel::ring3::enter_userland(hdr->e_entry, user_stack_top);
        return true;
    }

private:
    static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t len) {
        for (uint32_t i = 0; i < len; ++i) {
            dst[i] = src[i];
        }
    }
    static void zero_bytes(uint8_t* dst, uint32_t len) {
        for (uint32_t i = 0; i < len; ++i) {
            dst[i] = 0;
        }
    }
    static void debug_log(const char* msg);
    static void debug_log_fmt(const char* fmt, ...);
};

}
