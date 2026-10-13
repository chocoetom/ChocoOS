; socket(2)/connect(2)/read/write smoke test (Linux x86-64 ABI, ring 3).
; Connects to the host HTTP server at 10.0.2.2:18080 (QEMU user-net) and
; checks the reply starts with "HTTP".
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
    mov r12d, eax              ; fd

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
    mov eax, 1                 ; write
    syscall
    test eax, eax
    js .fail

    mov edi, r12d
    lea rsi, [rel buf]
    mov edx, 128
    mov eax, 0                 ; read (recv)
    syscall
    cmp eax, 4
    jl .fail
    mov eax, dword [rel buf]
    cmp eax, 0x50545448        ; "HTTP"
    jne .fail

    mov edi, r12d
    mov eax, 3                 ; close
    syscall
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 11
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231               ; exit_group(0)
    syscall
.fail:
    mov edi, 1
    lea rsi, [rel bad]
    mov edx, 11
    mov eax, 1
    syscall
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
; sockaddr_in { family=AF_INET, port=18080 (BE), addr=10.0.2.2 (BE), pad }
sa: db 2, 0, 0x46, 0xA0, 10, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0
req: db "GET / HTTP/1.0", 13, 10, "Host: 10.0.2.2", 13, 10, 13, 10
reqlen equ $ - req
ok: db "sock: PASS", 10
bad: db "sock: FAIL", 10
section .bss
align 16
buf: resb 128
