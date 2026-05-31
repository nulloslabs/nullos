#include <setjmp.h>

__attribute__((naked)) int setjmp(jmp_buf env) {
    __asm__ volatile (
        "mov [rdi], rbx\n\t"
        "mov [rdi + 8], rbp\n\t"
        "mov [rdi + 16], r12\n\t"
        "mov [rdi + 24], r13\n\t"
        "mov [rdi + 32], r14\n\t"
        "mov [rdi + 40], r15\n\t"
        "lea rax, [rsp + 8]\n\t"
        "mov [rdi + 48], rax\n\t"
        "mov rax, [rsp]\n\t"
        "mov [rdi + 56], rax\n\t"
        "xor rax, rax\n\t"
        "ret"
    );
}

__attribute__((naked, noreturn)) void longjmp(jmp_buf env, int val) {
    __asm__ volatile (
        "mov rax, rsi\n\t"
        "test rax, rax\n\t"
        "jnz 1f\n\t"
        "mov rax, 1\n\t"
        "1:\n\t"
        "mov rbx, [rdi]\n\t"
        "mov rbp, [rdi + 8]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r13, [rdi + 24]\n\t"
        "mov r14, [rdi + 32]\n\t"
        "mov r15, [rdi + 40]\n\t"
        "mov rsp, [rdi + 48]\n\t"
        "jmp [rdi + 56]"
    );
}
