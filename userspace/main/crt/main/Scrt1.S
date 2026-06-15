.intel_syntax noprefix
.section .text
.global _start
.type _start, @function

.extern main
.extern __libc_start_main
.weak _init
.weak _fini

_start:
    xor rbp, rbp
    mov r10, rsp                      // stack_end
    mov rsi, [rsp]                    // argc
    lea rdx, [rsp + 8]                // argv
    mov rdi, main@GOTPCREL[rip]       // main
    mov rcx, _init@GOTPCREL[rip]      // init
    mov r8, _fini@GOTPCREL[rip]       // fini
    xor r9d, r9d                      // rtld_fini
    and rsp, -16
    push 0                            // alignment padding
    push r10                          // stack_end
    call __libc_start_main@PLT
// Should be never reached
.loop:
    jmp .loop

.size _start, . - _start
