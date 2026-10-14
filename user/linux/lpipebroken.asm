; Verify the pipe write side reports EPIPE after the last reader closes.
bits 64
section .text
global _start
_start:
    lea rdi, [rel fds]
    mov eax, 22                 ; pipe
    syscall
    test eax, eax
    js .fail
    mov edi, [rel fds]          ; close read end
    mov eax, 3
    syscall
    lea rsi, [rel msg]
    mov edi, [rel fds + 4]
    mov edx, 1
    mov eax, 1                  ; write -> -EPIPE
    syscall
    cmp rax, -32
    jne .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 12
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall

section .rodata
msg: db "x"
ok: db "epipe: PASS", 10
section .data
align 8
fds: dd 0, 0
