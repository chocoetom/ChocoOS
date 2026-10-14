; /bin/lhello - Linux-ABI hello (ring 3, `syscall`, numbers from Linux).
; No libc: raw syscalls only. Proves the translator entry path.
bits 64
section .text
global _start
_start:
    mov rax, 1              ; sys_write
    mov rdi, 1
    lea rsi, [rel msg]
    mov rdx, msglen
    syscall
    cmp rax, 0
    jl .fail
    mov rax, 231            ; sys_exit_group
    xor rdi, rdi
    syscall
.fail:
    mov rax, 231
    mov rdi, 41
    syscall
section .rodata
msg: db "Hello from Linux ABI! syscall insn works.", 10
msglen: equ $ - msg
