#pragma once

#include <stdint.h>

namespace kernigham::loader {

// ELF header 32-bit
struct ELFHeader {
    uint8_t e_ident[16];      // ELF magic: 0x7F 'E' 'L' 'F'
    uint16_t e_type;           // e_type: ET_EXEC (2), ET_DYN (3)
    uint16_t e_machine;        // e_machine: EM_386 (3), EM_X86_64 (62)
    uint32_t e_version;        // version
    uint32_t e_entry;          // entry point (32-bit)
    uint32_t e_phoff;          // program header offset
    uint32_t e_shoff;          // section header offset
    uint32_t e_flags;          // flags
    uint16_t e_ehsize;         // ELF header size
    uint16_t e_phentsize;      // program header entry size
    uint16_t e_phnum;          // number of program headers
    uint16_t e_shentsize;      // section header entry size
    uint16_t e_shnum;          // number of section headers
    uint16_t e_shstrndx;       // section header string index
} __attribute__((packed));

// Program header 32-bit
struct ProgramHeader {
    uint32_t p_type;           // PT_LOAD (1), PT_DYNAMIC (3), etc.
    uint32_t p_offset;         // offset in file
    uint32_t p_vaddr;          // virtual address (load at this address)
    uint32_t p_paddr;          // physical address (ignored, same as vaddr)
    uint32_t p_filesz;         // size of segment in file
    uint32_t p_memsz;          // size of segment in memory (may have BSS)
    uint32_t p_flags;          // PF_X | PF_W | PF_R
    uint32_t p_align;          // alignment
} __attribute__((packed));

enum PT : uint32_t {
    PT_NULL = 0,
    PT_LOAD = 1,
    PT_DYNAMIC = 3,
};

enum PF : uint32_t {
    PF_X = 1,
    PF_W = 2,
    PF_R = 4,
};

enum EM : uint16_t {
    EM_386 = 3,
    EM_X86_64 = 62,
};

inline bool is_elf(const ELFHeader* hdr) {
    if (!hdr) return false;
    return hdr->e_ident[0] == 0x7F &&
           hdr->e_ident[1] == 'E' &&
           hdr->e_ident[2] == 'L' &&
           hdr->e_ident[3] == 'F';
}

}

