; ChocoOS - kernel context switch (64-bit, NASM)
; Callee-saved only: the interrupt Frames stay untouched on each stack,
; so switching inside the timer IRQ is transparent (iret restores IF).

bits 64
section .text

; void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
; rdi = &old_rsp, rsi = new_rsp
global switch_context
switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; First-time entry: r12 = Entry func, r13 = void* arg (set by sched::spawn).
; A fresh thread inherits IF=0 from its construction context; enabling
; interrupts here makes preemption live from the first instruction.
global thread_trampoline
extern choco_thread_exit
thread_trampoline:
    sti                        ; run thread code with preemption on
    mov rdi, r13
    call r12
    call choco_thread_exit
.hang:
    hlt
    jmp .hang
