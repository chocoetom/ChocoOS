; /bin/lbusypipe - `busybox echo hi | busybox wc -c` over a real kernel pipe.
; echo writes into pipe A (stdout); wc reads pipe A (stdin) and writes the byte
; count into pipe B (stdout); the parent reads pipe B and checks it got "3".
bits 64
section .text
global _start
_start:
    lea rdi, [rel pin]          ; pipe A: echo -> wc
    mov eax, 22
    syscall
    test eax, eax
    js .fail
    lea rdi, [rel pout]         ; pipe B: wc -> parent
    mov eax, 22
    syscall
    test eax, eax
    js .fail

    ; child 1: the writer (busybox echo hi)
    mov edi, 0x11               ; independent child
    lea rsi, [rel cstack1_end]
    mov eax, 56                 ; clone
    syscall
    test eax, eax
    jz .child1
    mov r12, rax

    ; child 2: the reader (busybox wc -c)
    mov edi, 0x11
    lea rsi, [rel cstack2_end]
    mov eax, 56
    syscall
    test eax, eax
    jz .child2
    mov r13, rax

    ; parent: drop every end except pipe B read, then read the count.
    mov edi, [rel pin]
    mov eax, 3
    syscall
    mov edi, [rel pin + 4]
    mov eax, 3
    syscall
    mov edi, [rel pout + 4]
    mov eax, 3
    syscall

    mov edi, [rel pout]
    lea rsi, [rel buf]
    mov edx, 16
    xor eax, eax                ; read
    syscall
    cmp rax, 0
    jle .fail
    mov al, [rel buf]
    cmp al, '3'                 ; "hi\n" == 3 bytes
    jne .fail

.wait1:
    mov rdi, r12
    lea rsi, [rel status]
    xor edx, edx
    xor r10d, r10d
    mov eax, 61
    syscall
    cmp rax, -11
    je .wait1
.wait2:
    mov rdi, r13
    lea rsi, [rel status]
    xor edx, edx
    xor r10d, r10d
    mov eax, 61
    syscall
    cmp rax, -11
    je .wait2

    mov edi, 1
    lea rsi, [rel ok]
    mov edx, 15
    mov eax, 1
    syscall
    xor edi, edi
    mov eax, 231
    syscall

.child1:
    mov edi, [rel pin + 4]      ; pipe A write end -> stdout
    mov esi, 1
    mov eax, 33                 ; dup2
    syscall
    mov edi, [rel pin]
    mov eax, 3
    syscall
    mov edi, [rel pin + 4]
    mov eax, 3
    syscall
    mov edi, [rel pout]
    mov eax, 3
    syscall
    mov edi, [rel pout + 4]
    mov eax, 3
    syscall
    lea rdi, [rel path]
    lea rsi, [rel argv1]
    xor edx, edx
    mov eax, 59                 ; execve
    syscall
    mov edi, 1
    mov eax, 231
    syscall

.child2:
    mov edi, [rel pin]          ; pipe A read end -> stdin
    xor esi, esi
    mov eax, 33
    syscall
    mov edi, [rel pout + 4]     ; pipe B write end -> stdout
    mov esi, 1
    mov eax, 33
    syscall
    mov edi, [rel pin]
    mov eax, 3
    syscall
    mov edi, [rel pin + 4]
    mov eax, 3
    syscall
    mov edi, [rel pout]
    mov eax, 3
    syscall
    mov edi, [rel pout + 4]
    mov eax, 3
    syscall
    lea rdi, [rel path]
    lea rsi, [rel argv2]
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
b0: db "busybox", 0
e:  db "echo", 0
h:  db "hi", 0
w:  db "wc", 0
c:  db "-c", 0
ok: db "busypipe: PASS", 10
section .data
align 8
argv1: dq b0, e, h, 0
argv2: dq b0, w, c, 0
pin:  dd 0, 0
pout: dd 0, 0
status: dd 0
section .bss
buf: resb 16
align 16
cstack1: resb 4096
cstack1_end:
cstack2: resb 4096
cstack2_end:
