; CPUID and RDTSC wrappers. Kept in asm because inline asm ties up rbx under
; clang even with -fno-pic, and CPUID clobbers it.
section .text
global choco_cpuid
; void choco_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4])
; CPUID clobbers eax/ebx/ecx/edx, so the out pointer (3rd arg, rdx) has to move
; to a register the instruction does not touch before executing it.
choco_cpuid:
    push rbx
    mov  eax, edi
    mov  ecx, esi
    mov  rsi, rdx
    cpuid
    mov  [rsi +  0], eax
    mov  [rsi +  4], ebx
    mov  [rsi +  8], ecx
    mov  [rsi + 12], edx
    pop  rbx
    ret

global choco_rdtsc
; uint64_t choco_rdtsc(void)
choco_rdtsc:
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    ret

global choco_cpuid_supported
; bool choco_cpuid_supported(void) - the ID bit in EFLAGS must be toggleable.
choco_cpuid_supported:
    pushfq
    pop  rax
    mov  rcx, rax
    xor  rax, 0x200000        ; try flipping bit 21
    push rax
    popfq
    pushfq
    pop  rax
    push rcx
    popfq                     ; restore
    xor  rax, rcx
    jz   .no
    mov  rax, 1
    ret
.no:
    xor  rax, rax
    ret
