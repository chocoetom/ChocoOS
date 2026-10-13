; Persistent volume: create the file on the first boot, verify it on the next.
bits 64
section .text
global _start
_start:
    mov edi, -100
    lea rsi, [rel path]
    xor edx, edx               ; O_RDONLY
    xor r10d, r10d
    mov eax, 257               ; openat
    syscall
    test eax, eax
    js .create

    mov r12, rax
    mov edi, eax
    lea rsi, [rel readback]
    mov edx, msglen
    xor eax, eax               ; read
    syscall
    cmp rax, msglen
    jne .fail_close
    mov edi, r12d
    mov eax, 3                 ; close
    syscall
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, oklen
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall

.create:
    mov edi, -100
    lea rsi, [rel path]
    mov edx, 0x241             ; O_WRONLY|O_CREAT|O_TRUNC
    mov r10d, 0644o
    mov eax, 257
    syscall
    test eax, eax
    js .fail
    mov r12, rax
    mov edi, eax
    lea rsi, [rel msg]
    mov edx, msglen
    mov eax, 1                 ; write
    syscall
    cmp rax, msglen
    jne .fail_close
    mov edi, r12d
    mov eax, 3
    syscall
    mov edi, 1
    lea rsi, [rel first]
    mov edx, firstlen
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall

.fail_close:
    mov edi, r12d
    mov eax, 3
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
path: db "keep.txt", 0
msg: db "persisted!"
msglen equ $ - msg
ok: db "persist: PASS", 10
oklen equ $ - ok
first: db "first boot", 10
firstlen equ $ - first
section .bss
readback: resb 16
