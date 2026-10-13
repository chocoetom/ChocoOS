; /bin/lfpu - per-thread FPU/SSE context test.
; The parent sums 1.0 (scalar double) 2^25 times -> exactly 2^25; a clone thread
; hammers the XMM registers with zeros the whole time. If the scheduler did not
; save/restore FPU state per thread, a preemption would hand the clobbered
; registers back to the parent and the sum would be wrong.
bits 64
section .text
global _start
_start:
    ; child: independent clone that clobbers the FPU in a tight loop
    mov edi, 0x11               ; SIGCHLD, no CLONE_VM
    lea rsi, [rel cstack_end]
    mov eax, 56                 ; clone
    syscall
    test eax, eax
    jz .child
    mov r12, rax

    pxor xmm0, xmm0             ; sum = 0.0
    movsd xmm1, [rel one]       ; addend = 1.0
    mov ecx, 0x1000000          ; 2^24 iterations
.ploop:
    addsd xmm0, xmm1
    dec ecx
    jnz .ploop
    movq r13, xmm0              ; keep the raw bit pattern

.pwait:
    mov rdi, r12
    lea rsi, [rel status]
    xor edx, edx
    xor r10d, r10d
    mov eax, 61                 ; wait4
    syscall
    cmp rax, -11
    je .pwait

    mov rax, 0x4170000000000000 ; 2^24 as IEEE-754 double
    cmp r13, rax
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

.child:
    mov ecx, 0x4000000          ; 2^26 iterations of clobbering (outlasts parent)
.cloop:
    xorps xmm0, xmm0
    xorps xmm1, xmm1
    dec ecx
    jnz .cloop
    xor edi, edi
    mov eax, 231
    syscall
section .rodata
one: dq 0x3FF0000000000000      ; 1.0 double
ok:  db "fpu: PASS", 10
section .data
align 8
status: dd 0
section .bss
align 16
cstack: resb 4096
cstack_end:
