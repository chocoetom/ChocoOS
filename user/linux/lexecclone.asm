; /bin/lexecclone - independent child execve probe.
bits 64
section .text
global _start
_start:
    lea rsi, [rel child_stack_end]
    mov edi, 0x11
    mov eax, 56
    syscall
    mov r12, rax
    test eax, eax
    jz .child
.wait:
    mov rdi, r12
    lea rsi, [rel status]
    xor edx, edx
    xor r10d, r10d
    mov eax, 61
    syscall
    cmp rax, -11
    je .wait
    cmp rax, r12
    jne .fail
    xor edi, edi
    mov eax, 231
    syscall
.child:
    lea rdi, [rel path]
    lea rsi, [rel argv]
    xor edx, edx
    mov eax, 59
    syscall
    mov edi, 1
    mov eax, 231
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall
section .data
path: db "/bin/lhello", 0
align 8
argv: dq path, 0
status: dd 0
section .bss
align 16
child_stack: resb 4096
child_stack_end:
