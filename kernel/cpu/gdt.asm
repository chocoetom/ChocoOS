; ChocoOS - load our own GDT + TSS (64-bit, NASM)
; void gdt_load(gdtr*, code_sel, data_sel, tss_sel)
; SysV: rdi=gdtr, rsi=code, rdx=data, rcx=tss

bits 64
section .text

global gdt_load
gdt_load:
    lgdt [rdi]
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push rsi
    lea rax, [rel .next]
    push rax
    retfq                 ; far return: reload CS
.next:
    ltr cx                ; load TSS selector
    ret
