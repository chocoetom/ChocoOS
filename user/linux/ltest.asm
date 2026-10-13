; /bin/ltest - Linux translator self-test (ring 3, no libc).
; Exercises the musl-startup subset: brk, mmap/munmap, arch_prctl,
; uname, clock_gettime, getrandom, writev. Prints PASS/FAIL, exit_group.
bits 64
section .text
global _start

; write(1, rsi, rdx)
print:
    mov rax, 1
    mov rdi, 1
    syscall
    ret

_start:
    ; --- brk(0) must return nonzero current break ---
    xor rdi, rdi
    mov rax, 12
    syscall
    test rax, rax
    jz .fail
    mov [rel b0], rax

    ; --- brk grow by 8192, touch, verify ---
    lea rdi, [rax + 8192]
    mov rax, 12
    syscall
    mov rbx, [rel b0]
    add rbx, 8100
    mov byte [rbx], 0x5A
    cmp byte [rbx], 0x5A
    jne .fail

    ; --- mmap anon 8192 at hint 0 ---
    xor rdi, rdi
    mov rsi, 8192
    mov rdx, 3
    mov r10, 0x22           ; MAP_PRIVATE|MAP_ANONYMOUS
    mov r8, -1
    xor r9, r9
    mov rax, 9
    syscall
    cmp rax, 0
    jl .fail
    mov [rel m0], rax
    ; touch first + last byte
    mov byte [rax], 0xA5
    mov rbx, rax
    add rbx, 8191
    mov byte [rbx], 0x5A
    cmp byte [rax], 0xA5
    jne .fail
    ; --- munmap it ---
    mov rdi, [rel m0]
    mov rsi, 8192
    mov rax, 11
    syscall
    test rax, rax
    jnz .fail

    ; --- arch_prctl SET_FS / GET_FS roundtrip ---
    lea rsi, [rel tcb]
    mov rdi, 0x1002
    mov rax, 158
    syscall
    test rax, rax
    jnz .fail
    lea rsi, [rel fsback]
    mov rdi, 0x1003
    mov rax, 158
    syscall
    test rax, rax
    jnz .fail
    mov rax, [rel fsback]
    lea rbx, [rel tcb]
    cmp rax, rbx
    jne .fail

    ; --- uname ---
    lea rdi, [rel uts]
    mov rax, 63
    syscall
    test rax, rax
    jnz .fail

    ; --- clock_gettime ---
    mov rdi, 1
    lea rsi, [rel ts]
    mov rax, 228
    syscall
    test rax, rax
    jnz .fail

    ; --- getrandom 16 ---
    lea rdi, [rel rnd]
    mov rsi, 16
    xor rdx, rdx
    mov rax, 318
    syscall
    cmp rax, 16
    jne .fail

    ; --- writev hello ---
    lea rsi, [rel iov]
    mov rdx, 2
    mov rdi, 1
    mov rax, 20
    syscall
    cmp rax, 0
    jle .fail

    lea rsi, [rel passmsg]
    mov rdx, passlen
    call print
    mov rax, 231
    xor rdi, rdi
    syscall
.fail:
    lea rsi, [rel failmsg]
    mov rdx, faillen
    call print
    mov rax, 231
    mov rdi, 1
    syscall

section .data
b0: dq 0
m0: dq 0
fsback: dq 0
tcb: times 64 db 0
uts: times 390 db 0
ts: dq 0, 0
rnd: times 16 db 0
iov: dq v1, v1len, v2, v2len
section .rodata
v1: db "ltest: writev works, "
v1len: equ $ - v1
v2: db "FS ok.", 10
v2len: equ $ - v2
passmsg: db "ltest: PASS", 10
passlen: equ $ - passmsg
failmsg: db "ltest: FAIL", 10
faillen: equ $ - failmsg
section .bss
