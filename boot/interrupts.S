.section .text
.global isr_unhandled_stub
.global isr_breakpoint_stub
.global irq0_stub
.global irq1_stub
.global irq2_stub
.global irq3_stub
.global irq4_stub
.global irq5_stub
.global irq6_stub
.global irq7_stub
.global irq8_stub
.global irq9_stub
.global irq10_stub
.global irq11_stub
.global irq12_stub
.global irq13_stub
.global irq14_stub
.global irq15_stub
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

irq1_stub:
    pusha
    cld
    push $33
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq2_stub:
    pusha
    cld
    push $34
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq3_stub:
    pusha
    cld
    push $35
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq4_stub:
    pusha
    cld
    push $36
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq5_stub:
    pusha
    cld
    push $37
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq6_stub:
    pusha
    cld
    push $38
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq7_stub:
    pusha
    cld
    push $39
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq8_stub:
    pusha
    cld
    push $40
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq9_stub:
    pusha
    cld
    push $41
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq10_stub:
    pusha
    cld
    push $42
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq11_stub:
    pusha
    cld
    push $43
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq12_stub:
    pusha
    cld
    push $44
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq13_stub:
    pusha
    cld
    push $45
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq14_stub:
    pusha
    cld
    push $46
    push %esp
    call irq_generic_handler
    add $8, %esp
    mov %eax, %esp
    popa
    iret

irq15_stub:
    pusha
    cld
    push $47
    push %esp
    call irq_generic_handler
    add $8, %esp
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
