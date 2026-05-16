; Minimal userland test program (32-bit)
; Demonstrates ring3 execution and syscall usage

BITS 32

; Entry point for userland
_start:
    ; Get PID via syscall
    mov eax, 7              ; kGetPid
    int 0x80
    mov ebx, eax            ; save PID in ebx

    ; Write "hello from userland\n" to stdout (fd=1)
    mov eax, 1              ; kWrite
    mov ebx, 1              ; fd=1 (stdout)
    mov ecx, msg            ; buffer address
    mov edx, msg_len        ; count
    int 0x80

    ; Sleep for 100 ticks
    mov eax, 3              ; kSleep
    mov ebx, 100            ; ticks
    int 0x80

    ; Exit with code 42
    mov eax, 2              ; kExit
    mov ebx, 42             ; exit code
    int 0x80

    ; Hang if we somehow return
    hlt
    jmp $

section .rodata
msg:
    db "hello from userland", 10
msg_len equ $ - msg
