; socketpair(AF_UNIX, SOCK_STREAM): write one end, read the other.
bits 64
section .text
global _start
_start:
    mov edi, 1                 ; AF_UNIX
    mov esi, 1                 ; SOCK_STREAM
    xor edx, edx
    lea r10, [rel sv]
    mov eax, 53                ; socketpair
    syscall
    test eax, eax
    js .fail
    mov edi, dword [rel sv]    ; end 0
    lea rsi, [rel msg]
    mov edx, 3
    mov eax, 1                 ; write
    syscall
    cmp eax, 3
    jne .fail
    mov edi, dword [rel sv + 4] ; end 1
    lea rsi, [rel buf]
    mov edx, 3
    xor eax, eax               ; read
    syscall
    cmp eax, 3
    jne .fail
    mov eax, dword [rel buf]
    and eax, 0xFFFFFF
    cmp eax, 0x636261          ; "abc"
    jne .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 12
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
msg: db "abc"
ok: db "spair: PASS", 10
section .bss
align 8
sv: resb 8
buf: resb 8
