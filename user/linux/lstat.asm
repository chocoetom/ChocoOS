; stat(2) metadata smoke test for an initrd regular file.
bits 64
section .text
global _start
_start:
    lea rdi, [rel path]
    lea rsi, [rel statbuf]
    mov eax, 4                 ; stat
    syscall
    test eax, eax
    js .fail
    mov eax, [rel statbuf + 24]
    and eax, 0170000o
    cmp eax, 0100000o          ; S_IFREG
    jne .fail
    cmp qword [rel statbuf + 48], 0
    je .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 11
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
path: db "motd.txt", 0
ok: db "stat: PASS", 10
section .bss
align 8
statbuf: resb 144
