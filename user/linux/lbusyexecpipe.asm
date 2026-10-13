; /bin/lbusyexecpipe - BusyBox child exec with inherited pipe redirection.
bits 64
section .text
global _start
_start:
    lea rdi, [rel fds]
    mov eax, 22                 ; pipe
    syscall
    test eax, eax
    js .fail
    mov edi, 0x11               ; independent child + SIGCHLD
    lea rsi, [rel child_stack_end]
    mov eax, 56                 ; clone
    syscall
    mov r12, rax
    test eax, eax
    jz .child

.read:
    mov edi, [rel fds]
    lea rsi, [rel out]
    mov edx, 16
    xor eax, eax
    syscall
    cmp rax, 12
    jne .read
    mov edi, 1
    lea rsi, [rel out]
    mov edx, 12
    mov eax, 1
    syscall

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
    mov edi, [rel fds + 4]
    mov esi, 1
    mov eax, 33                 ; dup2(pipe[1], stdout)
    syscall
    lea rdi, [rel path]
    lea rsi, [rel argv]
    xor edx, edx
    mov eax, 59                 ; execve
    syscall
    mov edi, 1
    mov eax, 231
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall

section .rodata
path: db "/bin/busybox", 0
arg0: db "busybox", 0
arg1: db "echo", 0
arg2: db "busyexec-ok", 0
section .data
align 8
argv: dq arg0, arg1, arg2, 0
fds: dd 0, 0
status: dd 0
section .bss
out: resb 16
align 16
child_stack: resb 4096
child_stack_end:
