; getrandom(2) smoke test for the kernel CSPRNG.
; Checks three things a real RNG must do:
;   1) return the full requested length (here 1024, exercising the raised cap),
;   2) not emit an all-zero / trivially-empty buffer,
;   3) return *different* bytes on successive calls. The old PIT-seeded LCG
;      re-seeded from the tick counter on every call, so two calls inside one
;      tick returned identical data; a proper DRBG advances its state.
bits 64
section .text
global _start
_start:
    ; 1) 1024 bytes -> must fill exactly, non-zero.
    lea rdi, [rel buf1]
    mov rsi, 1024
    xor edx, edx
    mov eax, 318               ; getrandom(buf, len, flags)
    syscall
    cmp rax, 1024
    jne .fail
    xor eax, eax
    mov ecx, 1024
    lea rsi, [rel buf1]
.sum:
    movzx edx, byte [rsi]
    add eax, edx
    inc rsi
    dec ecx
    jnz .sum
    test eax, eax
    jz .fail                   ; all-zero buffer

    ; 2) two 64-byte draws must differ.
    lea rdi, [rel buf2]
    mov rsi, 64
    xor edx, edx
    mov eax, 318
    syscall
    cmp rax, 64
    jne .fail
    lea rdi, [rel buf3]
    mov rsi, 64
    xor edx, edx
    mov eax, 318
    syscall
    cmp rax, 64
    jne .fail
    lea rsi, [rel buf2]
    lea rdi, [rel buf3]
    mov ecx, 64
.same:
    mov al, [rsi]
    cmp al, [rdi]
    jne .pass                  ; any differing byte proves the state advanced
    inc rsi
    inc rdi
    dec ecx
    jnz .same
    jmp .fail                  ; identical output => RNG not advancing

.pass:
    mov edi, 1
    lea rsi, [rel ok]
    mov edx, oklen
    mov eax, 1                 ; write(1, ok, len)
    syscall
    xor edi, edi
    mov eax, 231               ; exit_group(0)
    syscall
.fail:
    mov edi, 1
    mov eax, 231
    syscall
section .rodata
ok: db "rand: PASS", 10
oklen: equ $ - ok
section .bss
buf1: resb 1024
buf2: resb 64
buf3: resb 64
