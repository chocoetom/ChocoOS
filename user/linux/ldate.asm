; time(2) smoke test: the wall clock (CMOS RTC) must be later than 2023-11-14.
bits 64
section .text
global _start
_start:
    xor edi, edi
    mov eax, 201               ; time(NULL)
    syscall
    cmp rax, 1700000000        ; < 2023-11-14 -> RTC not read
    jl .fail
    mov rdi, 1
    lea rsi, [rel ok]
    mov edx, 11
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
ok: db "date: PASS", 10
