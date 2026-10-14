; mkdir + newfstatat directory type + rmdir smoke test.
bits 64
section .text
global _start
_start:
    lea rdi, [rel path]
    mov esi, 0755o
    mov eax, 83                 ; mkdir
    syscall
    test eax, eax
    js .fail
    mov edi, -100
    lea rsi, [rel path]
    lea rdx, [rel statbuf]
    xor r10d, r10d
    mov eax, 262                ; newfstatat
    syscall
    test eax, eax
    js .fail
    mov eax, [rel statbuf + 24]
    and eax, 0170000o
    cmp eax, 0040000o          ; S_IFDIR
    jne .fail
    lea rdi, [rel path]
    mov eax, 84                 ; rmdir
    syscall
    test eax, eax
    js .fail
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
path: db "work", 0
ok: db "mkdir: PASS", 10
section .bss
align 8
statbuf: resb 144
