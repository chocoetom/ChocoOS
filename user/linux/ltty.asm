; Linux ABI smoke test for the small controlling tty implementation.
bits 64
section .text
global _start
_start:
    xor edi, edi                ; stdin
    mov esi, 0x5401             ; TCGETS
    lea rdx, [rel termios]
    mov eax, 16                 ; ioctl
    syscall
    test eax, eax
    js .fail

    xor edi, edi
    mov esi, 0x5413             ; TIOCGWINSZ
    lea rdx, [rel winsize]
    mov eax, 16
    syscall
    test eax, eax
    js .fail
    cmp word [rel winsize], 25
    jne .fail
    cmp word [rel winsize + 2], 80
    jne .fail

    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 4
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
ok: db "tty", 10
section .bss
align 8
termios: resb 60
winsize: resb 8
