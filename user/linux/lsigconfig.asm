; Check rt_sigaction(SIGTERM, SIG_IGN) and rt_sigprocmask(SIGINT).
bits 64
section .text
global _start
_start:
    mov edi, 15                 ; SIGTERM
    lea rsi, [rel ignore]
    xor edx, edx
    mov r10d, 8                 ; sigsetsize
    mov eax, 13                 ; rt_sigaction
    syscall
    test eax, eax
    js .fail
    mov eax, 39                 ; getpid
    syscall
    mov edi, eax
    mov esi, 15
    mov eax, 62                 ; ignored SIGTERM must return
    syscall
    test eax, eax
    js .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 16
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
ok: db "sigconfig: PASS", 10
section .data
align 8
ignore: dq 1, 0, 0, 0
