; /bin/lclonepipe - shared-VM child writing through an inherited pipe.
bits 64
section .text
global _start
_start:
    lea rdi, [rel fds]
    mov eax, 22                 ; pipe
    syscall
    test eax, eax
    js .fail

    mov edi, 0x111              ; CLONE_VM | SIGCHLD
    lea rsi, [rel child_stack_end]
    mov eax, 56                 ; clone
    syscall
    mov r12, rax
    test eax, eax
    jz .child

.read:
    mov edi, [rel fds]
    lea rsi, [rel out]
    mov edx, msglen
    xor eax, eax                ; read
    syscall
    cmp rax, msglen
    jne .read

.wait:
    mov rdi, r12
    lea rsi, [rel status]
    xor edx, edx
    xor r10d, r10d
    mov eax, 61                 ; wait4
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
    mov edi, [rel fds + 4]
    lea rsi, [rel msg]
    mov edx, msglen
    mov eax, 1                 ; write
    syscall
    mov edi, 0
    mov eax, 231
    syscall

.fail:
    mov edi, 1
    mov eax, 231
    syscall

section .rodata
msg: db "child pipe", 10
msglen equ $ - msg
ok: db "clone/pipe/wait: PASS", 10
oklen equ $ - ok
section .data
align 8
fds: dd 0, 0
status: dd 0
section .bss
align 16
out: resb msglen
child_stack: resb 4096
child_stack_end:
