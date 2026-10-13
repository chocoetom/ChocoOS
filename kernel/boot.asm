; ChocoOS - 64-bit entry (Limine is already in long mode)
; Just calls kmain() and parks on hlt.

global _start
extern kmain

section .text
bits 64
_start:
    cli
    cld
    call kmain
.hang:
    hlt
    jmp .hang
