; /bin/lguard - glibc pointer-guard call isolation (ring 3, no libc).
; SET_FS -> store guard -> call through pointer_guard demangle pattern.
bits 64
section .text
global _start
_start:
    ; arch_prctl(ARCH_SET_FS, tcb)
    lea rsi, [rel tcb]
    mov rdi, 0x1002
    mov rax, 158
    syscall

    ; store a fake "guard" at fs:[0x30] and a function pointer
    mov rax, 0xDEADBEEFCAFEBABE
    mov rdx, 0x30
    mov fs:[rdx], rax
    lea rax, [rel target]
    mov rbx, rax          ; rbx = plain pointer (matches BusyBox dump)

    ; glibc pattern: mangled = rol(ptr ^ guard, 17), then demangle.
    mov rdx, 0xDEADBEEFCAFEBABE
    mov rax, rbx
    xor rax, rdx
    rol rax, 17
    ror rax, 17
    mov rcx, 0x30
    xor rax, fs:[rcx]
    call rax

    ; if we get here, print OK
    lea rsi, [rel okmsg]
    mov rdx, oklen
    mov rdi, 1
    mov rax, 1
    syscall
    mov rax, 231
    xor rdi, rdi
    syscall
target:
    ret

section .data
okmsg: db "lguard: pointer-guard call works.", 10
oklen: equ $ - okmsg
section .bss
tcb: times 64 db 0
