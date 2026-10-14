; mmap(2) file-backed smoke test: map proc/version and read it.
bits 64
section .text
global _start
_start:
    mov edi, -100              ; AT_FDCWD
    lea rsi, [rel path]
    xor edx, edx
    xor r10d, r10d
    mov eax, 257               ; openat
    syscall
    test eax, eax
    js .fail
    mov r12d, eax              ; fd

    xor edi, edi               ; addr = NULL
    mov esi, 64                ; len
    mov edx, 1                 ; PROT_READ
    mov r10d, 2                ; MAP_PRIVATE
    mov r8d, r12d              ; fd
    xor r9d, r9d               ; offset
    mov eax, 9                 ; mmap
    syscall
    cmp rax, 0
    jle .fail                  ; error / NULL -> fail
    mov rbx, rax
    mov eax, dword [rbx]       ; content must start with "Choc"
    cmp eax, 0x636F6843
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
path: db "proc/version", 0
ok: db "mmap: PASS", 10
