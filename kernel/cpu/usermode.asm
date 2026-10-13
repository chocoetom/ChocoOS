; ChocoOS - enter ring 3 (64-bit, NASM)
; void user_enter(uint64_t entry, uint64_t stack_top)
; SysV: rdi = user RIP, rsi = user RSP (top, 16-byte aligned)
; Selectors must match gdt.cpp: UCODE=0x18, UDATA=0x20 (+3 for RPL3).

bits 64
section .text

global user_enter
global user_resume
user_enter:
    push qword 0x23        ; SS = UDATA|3
    push rsi               ; user RSP
    push qword 0x202       ; RFLAGS (IF=1, bit1=1)
    push qword 0x1B        ; CS = UCODE|3
    push rdi               ; user RIP
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; A fresh Linux process starts with no inherited kernel register state.
    ; In particular glibc treats RDX as rtld_fini and registers it at exit.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq                  ; -> ring 3, never returns here

; void user_resume(uint64_t entry, uint64_t stack_top)
; Starts a clone child at the post-syscall continuation with rax=0.
user_resume:
    mov rsp, rsi
    push qword 0x23
    push rsi
    push qword 0x202
    push qword 0x1B
    push rdi
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq
