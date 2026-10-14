; ChocoOS - Linux syscall entry (64-bit, NASM)
; MSR_LSTAR -> linux_syscall_entry. Entered by the `syscall` instruction
; from ring 3: RCX = user RIP, R11 = user RFLAGS, RSP = user RSP.
; Builds the same choco::cpu::Frame as isr.asm (r15..rax, vec, err, rip, cs,
; rflags, rsp, ss) and calls linux_dispatch. Returns via iretq (no GDT
; reorder needed for SYSRET).
;
; NOTE: single-core design, like the shared rsp0. A syscall runs with IF masked,
; so normally it cannot be preempted; but the blocking paths re-enable IF, and
; then another thread's syscall would overwrite this frame. The scheduler
; therefore points linux_syscall_rsp at the running thread's OWN syscall stack
; (see sched::do_switch); the static one below is the boot/idle default.

bits 64
section .text

global linux_syscall_entry
extern linux_dispatch

section .bss
align 16
syscall_stack:
    resb 16384
syscall_stack_top:
tmp_user_rsp:
    resq 1

section .data
align 8
global linux_syscall_rsp
linux_syscall_rsp:
    dq syscall_stack_top

section .text
linux_syscall_entry:
    cli                          ; keep the single syscall stack private
    mov [rel tmp_user_rsp], rsp   ; still the user stack here
    mov rsp, [rel linux_syscall_rsp]
    ; Build the same layout as the CPU + isr_common frame:
    ;   r15..rax, vec, err, rip, cs, rflags, rsp, ss
    ; RCX=user RIP and R11=user RFLAGS after syscall.  Linux does not
    ; preserve either register across the syscall, so they can fill the
    ; corresponding saved-register slots as well.
    push qword 0x23               ; ss = UDATA|3
    push qword [rel tmp_user_rsp] ; user rsp
    push r11                      ; user rflags
    push qword 0x1B               ; cs = UCODE|3
    push rcx                      ; user rip
    push qword 0                  ; err (dummy)
    push qword 0x100              ; vec (linux marker)
    push rax                      ; syscall number
    push rbx
    push rcx                      ; syscall clobbers RCX; Linux uses R10 for arg 4
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10                      ; Linux 4th arg
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp                  ; Frame* -> 1st arg (SysV)
    call linux_dispatch
    mov [rsp + 14*8], rax         ; frame.rax = return value
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                   ; drop ss+rsp
    iretq                         ; back to ring 3 (loads user rsp)
