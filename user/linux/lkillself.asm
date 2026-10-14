; Basic Linux kill(2) termination path: SIGTERM becomes status 128+15.
bits 64
section .text
global _start
_start:
    mov edi, 1
    lea rsi, [rel msg]
    mov edx, 14
    mov eax, 1                  ; write marker
    syscall
    mov eax, 39                 ; getpid
    syscall
    mov edi, eax                ; pid
    mov esi, 15                 ; SIGTERM
    mov eax, 62                 ; kill
    syscall                     ; does not return
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
msg: db "signal: PASS?", 10
