; /bin/lclone - CLONE_VM + wait4 probe.
bits 64
section .text
global _start
_start:
    mov edi, 0x111              ; CLONE_VM | SIGCHLD
    lea rsi, [rel child_stack_end]
    mov eax, 56                 ; clone
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
    jne .wait_done
    jmp .wait

.wait_done:
    lea rsi, [rel ok]
    mov edx, oklen
    mov edi, 1
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall

.child:
    mov edi, 7
    mov eax, 231
    syscall

section .data
ok: db "clone/wait: PASS", 10
oklen equ $ - ok
section .bss
status: resd 1
align 16
child_stack: resb 4096
child_stack_end:
