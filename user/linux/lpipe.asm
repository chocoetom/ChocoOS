; /bin/lpipe - minimal Linux pipe2/read/write probe.
bits 64
section .text
global _start
_start:
    lea rdi, [rel fds]
    xor esi, esi
    mov eax, 293                 ; pipe2
    syscall
    test eax, eax
    js .exit

    mov edi, [rel fds + 4]
    lea rsi, [rel msg]
    mov edx, msglen
    mov eax, 1                   ; write(pipe[1], ...)
    syscall

    ; Redirect stdin to the pipe and read through fd 0 as dup2 coverage.
    mov edi, [rel fds]
    xor esi, esi
    mov eax, 33                  ; dup2(pipe[0], 0)
    syscall
    lea rsi, [rel out]
    mov edx, msglen
    xor edi, edi
    xor eax, eax
    syscall

    mov edi, 1
    lea rsi, [rel out]
    mov edx, msglen
    mov eax, 1
    syscall
.exit:
    xor edi, edi
    mov eax, 231
    syscall

section .data
msg: db "pipe: PASS", 10
msglen equ $ - msg
section .bss
fds: resd 2
out: resb msglen
