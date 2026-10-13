; AF_UNIX by path: bind/listen on a named endpoint, then a second socket
; connect()s to it. The client writes PING, the accepted connection reads it,
; answers PONG and the client reads that back.
bits 64
section .text
global _start
_start:
    ; server = socket(AF_UNIX=1, SOCK_STREAM=1, 0)
    mov edi, 1
    mov esi, 1
    xor edx, edx
    mov eax, 41                 ; socket
    syscall
    test eax, eax
    js .fail
    mov r12d, eax               ; server fd

    ; bind(server, &sun, 110)
    mov edi, r12d
    lea rsi, [rel sun]
    mov edx, 110
    mov eax, 49                 ; bind
    syscall
    test eax, eax
    js .fail

    ; listen(server, 1)
    mov edi, r12d
    mov esi, 1
    mov eax, 50                 ; listen
    syscall
    test eax, eax
    js .fail

    ; client = socket(AF_UNIX, SOCK_STREAM, 0)
    mov edi, 1
    mov esi, 1
    xor edx, edx
    mov eax, 41                 ; socket
    syscall
    test eax, eax
    js .fail
    mov r13d, eax               ; client fd

    ; connect(client, &sun, 110)
    mov edi, r13d
    lea rsi, [rel sun]
    mov edx, 110
    mov eax, 42                 ; connect
    syscall
    test eax, eax
    js .fail

    ; write(client, "PING", 4)
    mov edi, r13d
    lea rsi, [rel ping]
    mov edx, 4
    mov eax, 1                  ; write
    syscall
    cmp eax, 4
    jne .fail

    ; conn = accept(server, NULL, 0)
    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov eax, 43                 ; accept
    syscall
    test eax, eax
    js .fail
    mov r14d, eax               ; conn fd

    ; read(conn, buf, 8) -> "PING"
    mov edi, r14d
    lea rsi, [rel buf]
    mov edx, 8
    xor eax, eax                ; read
    syscall
    cmp eax, 4
    jne .fail
    mov eax, dword [rel buf]
    cmp eax, 0x474E4950         ; "PING"
    jne .fail

    ; write(conn, "PONG", 4)
    mov edi, r14d
    lea rsi, [rel pong]
    mov edx, 4
    mov eax, 1                  ; write
    syscall
    cmp eax, 4
    jne .fail

    ; read(client, buf, 8) -> "PONG"
    mov edi, r13d
    lea rsi, [rel buf]
    mov edx, 8
    xor eax, eax                ; read
    syscall
    cmp eax, 4
    jne .fail
    mov eax, dword [rel buf]
    cmp eax, 0x474E4F50         ; "PONG"
    jne .fail

    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 12
    mov eax, 1                  ; write
    syscall
    xor edi, edi
    mov eax, 231                ; exit_group
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
align 2
sun:
    dw 1                        ; AF_UNIX
    db "/tmp/chocosock", 0
ping:   db "PING"
pong:   db "PONG"
ok:     db "usock: PASS", 10
section .bss
align 8
buf:    resb 16
