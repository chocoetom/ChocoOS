; In-memory writable VFS smoke test: create, write, reopen, read, unlink.
bits 64
section .text
global _start
_start:
    mov edi, -100
    lea rsi, [rel path]
    mov edx, 0x242             ; O_RDWR|O_CREAT|O_TRUNC
    mov r10d, 0644o
    mov eax, 257               ; openat
    syscall
    test eax, eax
    js .fail
    mov r12, rax
    mov edi, eax
    lea rsi, [rel data]
    mov edx, 5
    mov eax, 1                 ; write
    syscall
    cmp rax, 5
    jne .fail_close
    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov eax, 8                 ; lseek
    syscall
    mov edi, r12d
    lea rsi, [rel readback]
    mov edx, 5
    xor eax, eax                ; read
    syscall
    cmp rax, 5
    jne .fail_close
    mov edi, r12d
    mov eax, 3                 ; close
    syscall
    mov edi, -100
    lea rsi, [rel path]
    xor edx, edx               ; O_RDONLY
    xor r10d, r10d
    mov eax, 257
    syscall
    test eax, eax
    js .fail
    mov r12, rax
    mov edi, r12d
    lea rsi, [rel readback]
    mov edx, 5
    xor eax, eax
    syscall
    cmp rax, 5
    jne .fail_close
    mov edi, r12d
    mov eax, 3
    syscall
    lea rdi, [rel path]
    mov eax, 87               ; unlink
    syscall
    test eax, eax
    js .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 9
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
path: db "tmp.txt", 0
data: db "hello"
ok: db "fs: PASS", 10
section .bss
readback: resb 8
