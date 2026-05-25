.section .text
.global gdt_flush
.global tss_flush
.type gdt_flush, @function
.type tss_flush, @function

gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $gdt_flush_done
    
gdt_flush_done:
    ret

tss_flush:
    mov $0x28, %ax
    ltr %ax
    ret

.size gdt_flush, . - gdt_flush
.size tss_flush, . - tss_flush

.section .note.GNU-stack,"",@progbits
