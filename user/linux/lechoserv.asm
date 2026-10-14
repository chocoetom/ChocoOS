; TCP echo server (single connection): bind 0.0.0.0:18081, listen, accept,
; read a request and reply "PONG". Verifies the passive-open path.
bits 64
section .text
global _start
_start:
    mov edi, 2                 ; AF_INET
    mov esi, 1                 ; SOCK_STREAM
    xor edx, edx
    mov eax, 41                ; socket
    syscall
    test eax, eax
    js .fail
    mov r12d, eax

    mov edi, r12d
    lea rsi, [rel sa]
    mov edx, 16
    mov eax, 49                ; bind
    syscall
    test eax, eax
    js .fail

    mov edi, r12d
    mov esi, 4
    mov eax, 50                ; listen
    syscall
    test eax, eax
    js .fail
    mov edi, 1
    lea rsi, [rel m_listen]
    mov edx, 11
    mov eax, 1
    syscall

    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov eax, 43                ; accept
    syscall
    test eax, eax
    js .fail
    mov r13d, eax
    mov edi, 1
    lea rsi, [rel m_accept]
    mov edx, 11
    mov eax, 1
    syscall

    mov edi, r13d
    lea rsi, [rel buf]
    mov edx, 64
    xor eax, eax               ; read request
    syscall
    test eax, eax
    js .fail

    mov edi, r13d
    lea rsi, [rel pong]
    mov edx, 5
    mov eax, 1                 ; reply
    syscall

    mov edi, 1
    lea rsi, [rel m_done]
    mov edx, 9
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall
.fail:
    mov edi, 1
    lea rsi, [rel m_fail]
    mov edx, 9
    mov eax, 1
    syscall
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
; sockaddr_in { AF_INET, port 18081 (BE), 0.0.0.0 }
sa: db 2, 0, 0x46, 0xA1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
pong: db "PONG", 10
m_listen: db "SRV listen", 10
m_accept: db "SRV accept", 10
m_done:   db "SRV done", 10
m_fail:   db "SRV FAIL", 10
section .bss
align 16
buf: resb 64
