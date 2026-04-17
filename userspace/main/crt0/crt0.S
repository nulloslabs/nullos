.intel_syntax noprefix
.section .text
.global _start

_start:
    xor rbp, rbp
    mov rdi, [rsp]                    // argc
    lea rsi, [rsp + 8]                // argv
    lea rdx, [rsp + 8 + rdi*8] // envp
    and rsp, -16
    call main
    mov rdi, rax
    mov rax, 0
    syscall
// Should be never reached
.loop:
    jmp .loop