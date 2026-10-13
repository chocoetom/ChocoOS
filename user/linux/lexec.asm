; /bin/lexec - execve replacement probe.
bits 64
section .text
global _start
_start:
    lea rdi, [rel path]
    lea rsi, [rel argv]
    xor edx, edx
    mov eax, 59                 ; execve
    syscall
    mov edi, 1                  ; failure is visible if exec returns
    mov eax, 231
    syscall
section .data
path: db "/bin/lhello", 0
argv: dq path, 0
