; poll(2) smoke test: after an HTTP GET the socket must become POLLIN.
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
    mov eax, 42                ; connect
    syscall
    test eax, eax
    js .fail

    mov edi, r12d
    lea rsi, [rel req]
    mov edx, reqlen
    mov eax, 1                 ; write GET
    syscall
    test eax, eax
    js .fail

    mov dword [rel pfd], r12d  ; pollfd.fd
    mov word [rel pfd + 4], 1  ; .events = POLLIN
    mov word [rel pfd + 6], 0  ; .revents
    lea rdi, [rel pfd]
    mov esi, 1
    mov edx, 5000              ; 5s
    mov eax, 7                 ; poll
    syscall
    cmp eax, 1
    jne .fail
    mov ax, word [rel pfd + 6]
    test ax, 1                 ; POLLIN
    jz .fail

    mov edi, r12d
    lea rsi, [rel buf]
    mov edx, 128
    mov eax, 0                 ; read
    syscall
    cmp eax, 4
    jl .fail
    mov eax, dword [rel buf]
    cmp eax, 0x50545448        ; "HTTP"
    jne .fail

    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 10
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
sa: db 2, 0, 0x46, 0xA0, 10, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0
req: db "GET / HTTP/1.0", 13, 10, "Host: 10.0.2.2", 13, 10, 13, 10
reqlen equ $ - req
ok: db "poll: PASS", 10
section .bss
align 8
pfd: resb 8
buf: resb 128
