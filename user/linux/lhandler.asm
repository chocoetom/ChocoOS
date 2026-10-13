; Synchronous self-signal handler smoke test.
bits 64
section .text
global _start
_start:
    mov edi, 15                 ; SIGTERM
    lea rsi, [rel action]
    xor edx, edx
    mov r10d, 8
    mov eax, 13                 ; rt_sigaction
    syscall
    test eax, eax
    js bad
    mov eax, 39                 ; getpid
    syscall
    mov edi, eax
    mov esi, 15
    mov eax, 62                 ; kill(self, SIGTERM)
    syscall                     ; kernel redirects to signal_handler
    mov edi, 1
    lea rsi, [rel returned]
    mov edx, 13
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall

signal_handler:
    mov edi, 1
    lea rsi, [rel msg]
    mov edx, 14
    mov eax, 1
    syscall
    mov eax, 15                 ; rt_sigreturn
    syscall
bad:
    mov edi, 1
    mov eax, 231
    syscall

section .rodata
msg: db "handler: PASS", 10
returned: db "return: PASS", 10
section .data
align 8
action: dq signal_handler, 0, 0, 0
