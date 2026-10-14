; ChocoOS user entry (ring 3). Stack on entry: [rsp]=argc, [rsp+8]=argv.
; Calls main(argc, argv), exits with its return code.
bits 64
section .text
global _start
extern main
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp+8]        ; argv (char **, NULL-terminated)
    call main               ; rax = exit code
    mov rdi, rax
    mov rax, 0              ; SYS_EXIT
    int 0x80
.hang:
    hlt
    jmp .hang
