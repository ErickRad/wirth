# Ring3 entry using IRET
.section .text
.global enter_ring3_simple
.type enter_ring3_simple, @function

enter_ring3_simple:
    mov 4(%esp), %eax
    mov 8(%esp), %edx
    
    mov $0x23, %ecx
    mov %ecx, %ds
    mov %ecx, %es
    mov %ecx, %fs
    mov %ecx, %gs
    
    mov $0x23, %ecx
    push %ecx
    push %edx
    mov $0x202, %ecx
    push %ecx
    mov $0x1B, %ecx
    push %ecx
    push %eax
    
    iret

.size enter_ring3_simple,.-enter_ring3_simple
.section .note.GNU-stack,"",@progbits
