.intel_syntax noprefix
.section .text
.global _start

.extern environ

// We can include this safely because GCC runs the C preprocessor
#include <sys/syscall.h>

_start:
    xor rbp, rbp
    mov rdi, [rsp]                    // argc
    lea rsi, [rsp + 8]                // argv
    lea rdx, [rsp + 8 + rdi*8 + 8]    // envp
    mov [rip + environ], rdx
    and rsp, -16
    call main
    mov rdi, rax
    mov rax, SYS_exit
    syscall
// Should be never reached
.loop:
    jmp .loop
