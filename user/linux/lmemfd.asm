; memfd_create + ftruncate + MAP_SHARED mmap smoke test.
; Proves the backing store Wayland's wl_shm needs: an anonymous object that can
; be resized, mapped, and SHARED so a write through one mapping is visible
; through another (the compositor reads what the client wrote).
bits 64
section .text
global _start
_start:
    ; fd = memfd_create(NULL, 0)
    xor edi, edi
    xor esi, esi
    mov eax, 319
    syscall
    cmp rax, 3
    jl .fail
    mov r12, rax               ; r12 = fd

    ; ftruncate(fd, 4096)
    mov rdi, r12
    mov esi, 4096
    mov eax, 77
    syscall
    test rax, rax
    jne .fail

    ; a1 = mmap(NULL, 4096, PROT_RW=3, MAP_SHARED=1, fd, 0)
    xor edi, edi
    mov esi, 4096
    mov edx, 3
    mov r10d, 1
    mov r8, r12
    xor r9d, r9d
    mov eax, 9
    syscall
    cmp rax, 0
    jle .fail
    mov r13, rax               ; r13 = a1

    ; a2 = mmap(NULL, 4096, PROT_RW=3, MAP_SHARED=1, fd, 0)
    xor edi, edi
    mov esi, 4096
    mov edx, 3
    mov r10d, 1
    mov r8, r12
    xor r9d, r9d
    mov eax, 9
    syscall
    cmp rax, 0
    jle .fail
    mov r14, rax               ; r14 = a2

    ; Write a pattern through a1.
    mov byte [r13], 0xAB
    mov byte [r13 + 10], 0xCD
    mov byte [r13 + 4095], 0xEF

    ; Same mapping: read back through a1.
    mov al, [r13]
    cmp al, 0xAB
    jne .fail
    mov al, [r13 + 4095]
    cmp al, 0xEF
    jne .fail

    ; SHARED: the write must be visible through the second mapping a2.
    mov al, [r14 + 10]
    cmp al, 0xCD
    jne .fail
    mov al, [r14]
    cmp al, 0xAB
    jne .fail

    ; And a write through a2 must be visible through a1.
    mov byte [r14 + 100], 0x11
    mov al, [r13 + 100]
    cmp al, 0x11
    jne .fail

    mov edi, 1
    lea rsi, [rel ok]
    mov edx, oklen
    mov eax, 1                 ; write(1, ok, len)
    syscall
    xor edi, edi
    mov eax, 231               ; exit_group(0)
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
ok: db "memfd: PASS", 10
oklen: equ $ - ok
