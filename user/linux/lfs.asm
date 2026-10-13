; /bin/lfs - FS segment probe (ring 3, no libc).
; arch_prctl(SET_FS) then read fs:[0x30] (pthread ptr) and fs:[0x00] (tcb self).
bits 64
section .text
global _start
_start:
    ; arch_prctl(ARCH_SET_FS, tcb)
    lea rsi, [rel tcb]
    mov rdi, 0x1002
    mov rax, 158
    syscall

    ; fs:[0x00] -> rax (self-pointer = fsbase)
    xor eax, eax
    mov rbx, fs:[rax]
    mov [rel fs00], rbx

    ; fs:[0x30] -> rax (stack_guard)
    mov eax, 0x30
    mov rbx, fs:[rax]
    mov [rel fs30], rbx

    ; print both (16 raw bytes)
    mov rdi, 1
    lea rsi, [rel fs00]
    mov rdx, 16
    mov rax, 1
    syscall

    ; exit_group(0)
    mov rax, 231
    xor rdi, rdi
    syscall

section .data
fs30: dq 0
fs00: dq 0
section .bss
tcb: times 64 db 0
