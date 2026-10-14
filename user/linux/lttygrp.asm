; TTY foreground process-group smoke test.
bits 64
section .text
global _start
_start:
    xor edi, edi
    mov esi, 0x540F             ; TIOCGPGRP
    lea rdx, [rel pgid]
    mov eax, 16
    syscall
    test eax, eax
    js .fail
    cmp dword [rel pgid], 0
    je .fail
    xor edi, edi
    mov esi, 0x5410             ; TIOCSPGRP
    lea rdx, [rel pgid]
    mov eax, 16
    syscall
    test eax, eax
    js .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 13
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
ok: db "ttygrp: PASS", 10
section .bss
align 4
pgid: resd 1
