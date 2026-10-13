; /bin/lfork - independent address-space clone probe.
bits 64
section .text
global _start
_start:
    lea rsi, [rel child_stack_end]
    mov edi, 0x11              ; SIGCHLD, no CLONE_VM
    mov eax, 56
    syscall
    mov r12, rax
    test eax, eax
    jz .child
.wait:
    mov rdi, r12
    lea rsi, [rel status]
    xor edx, edx
    xor r10d, r10d
    mov eax, 61
    syscall
    cmp rax, -11
    je .wait
    cmp rax, r12
    jne .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, oklen
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall
.child:
    mov edi, 9
    mov eax, 231
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
ok: db "fork/wait: PASS", 10
oklen equ $ - ok
section .data
status: dd 0
section .bss
align 16
child_stack: resb 4096
child_stack_end:
