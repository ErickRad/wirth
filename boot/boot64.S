.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH, 0
.set MB2_HEADER_LEN, mb2_header_end - mb2_header_start
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

.section .multiboot, "a"
.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM

    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .text
.code32
.global _start
.type _start, @function
.extern kernel_main64
_start:
    cli
    mov $stack64_top, %esp
    mov %eax, mb_magic32
    mov %ebx, mb_info32

    lgdt gdt64_ptr

    mov $pml4, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    or $0x00000100, %eax
    wrmsr

    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    jmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    lea stack64_top(%rip), %rsp
    and $-16, %rsp

    mov mb_magic32(%rip), %edi
    mov mb_info32(%rip), %esi
    call kernel_main64

halt64:
    hlt
    jmp halt64

.size _start, . - _start

.section .data
.align 4096
pml4:
    .quad pdpt + 0x03
    .fill 511, 8, 0
pdpt:
    .quad pd + 0x03
    .fill 511, 8, 0
pd:
    .quad 0x00000083
    .fill 511, 8, 0

gdt64:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00AF92000000FFFF
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

mb_magic32:
    .long 0
mb_info32:
    .long 0

.section .bss
.align 16
stack64_bottom:
    .skip 16384
stack64_top:

.section .note.GNU-stack,"",@progbits
