; Nested directory and file path smoke test.
bits 64
section .text
global _start
_start:
    lea rdi, [rel home]
    mov esi, 0755o
    mov eax, 83
    syscall
    test eax, eax
    js .fail
    lea rdi, [rel userdir]
    mov esi, 0755o
    mov eax, 83
    syscall
    test eax, eax
    js .fail
    mov edi, -100
    lea rsi, [rel file_a]
    mov edx, 0x42               ; O_WRONLY|O_CREAT
    mov r10d, 0644o
    mov eax, 257
    syscall
    test eax, eax
    js .fail
    mov r12, rax
    mov edi, eax
    lea rsi, [rel data]
    mov edx, 1
    mov eax, 1
    syscall
    cmp rax, 1
    jne .fail_close
    mov edi, r12d
    mov eax, 3
    syscall
    lea rdi, [rel file_a]
    lea rsi, [rel file_b]
    mov eax, 82
    syscall
    test eax, eax
    js .fail
    lea rdi, [rel file_b]
    mov eax, 87
    syscall
    test eax, eax
    js .fail
    lea rdi, [rel userdir]
    mov eax, 84
    syscall
    test eax, eax
    js .fail
    lea rdi, [rel home]
    mov eax, 84
    syscall
    test eax, eax
    js .fail
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 11
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
home: db "home", 0
userdir: db "home/user", 0
file_a: db "home/user/a", 0
file_b: db "home/user/b", 0
data: db "x"
ok: db "hier: PASS", 10
