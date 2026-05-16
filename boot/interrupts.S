.section .text
.global isr_unhandled_stub
.global isr_breakpoint_stub
.global irq0_stub
.global syscall_stub
.global isr_gp_stub
.global isr_pf_stub
.type isr_unhandled_stub, @function
.type isr_breakpoint_stub, @function
.type irq0_stub, @function
.type syscall_stub, @function
.type isr_gp_stub, @function
.type isr_pf_stub, @function

isr_unhandled_stub:
    pusha
    cld
    call unhandled_interrupt_handler
    popa
    iret

isr_breakpoint_stub:
    pusha
    cld
    call breakpoint_handler
    popa
    iret

irq0_stub:
    pusha
    cld
    push %esp
    call irq0_handler
    add $4, %esp
    mov %eax, %esp
    popa
    iret

syscall_stub:
    pusha
    cld
    mov 28(%esp), %eax
    mov 16(%esp), %ebx
    mov 24(%esp), %ecx
    mov 20(%esp), %edx
    push %edx
    push %ecx
    push %ebx
    push %eax
    call syscall_dispatch
    add $16, %esp
    mov %eax, 28(%esp)
    popa
    iret

isr_gp_stub:
    pusha
    cld
    mov 32(%esp), %eax
    push %eax
    push $13
    call exception_handler
    add $8, %esp
    popa
    add $4, %esp
    iret

isr_pf_stub:
    pusha
    cld
    mov 32(%esp), %eax
    push %eax
    push $14
    call exception_handler
    add $8, %esp
    popa
    add $4, %esp
    iret

.size isr_unhandled_stub, . - isr_unhandled_stub
.size isr_breakpoint_stub, . - isr_breakpoint_stub
.size irq0_stub, . - irq0_stub
.size syscall_stub, . - syscall_stub
.size isr_gp_stub, . - isr_gp_stub
.size isr_pf_stub, . - isr_pf_stub

.section .note.GNU-stack,"",@progbits
