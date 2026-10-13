; fstat(2) smoke test: open an initrd file and stat its descriptor.
; Regression for the fstat ABI (statbuf comes in RSI, not RDI).
bits 64
section .text
global _start
_start:
    mov edi, -100              ; AT_FDCWD
    lea rsi, [rel path]
    xor edx, edx               ; O_RDONLY
    xor r10d, r10d
    mov eax, 257               ; openat
    syscall
    test eax, eax
    js .fail
    mov edi, eax               ; fd
    lea rsi, [rel statbuf]
    mov eax, 5                 ; fstat
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
path: db "motd.txt", 0
ok: db "fstat: PASS", 10
section .bss
align 8
statbuf: resb 144
