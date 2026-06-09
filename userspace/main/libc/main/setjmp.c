#include <setjmp.h>

__attribute__((naked)) int setjmp(jmp_buf env) {
    __asm__ volatile (
        "movq %rbx, (%rdi)\n\t"
        "movq %rbp, 8(%rdi)\n\t"
        "movq %r12, 16(%rdi)\n\t"
        "movq %r13, 24(%rdi)\n\t"
        "movq %r14, 32(%rdi)\n\t"
        "movq %r15, 40(%rdi)\n\t"
        "leaq 8(%rsp), %rax\n\t"
        "movq %rax, 48(%rdi)\n\t"
        "movq (%rsp), %rax\n\t"
        "movq %rax, 56(%rdi)\n\t"
        "xorq %rax, %rax\n\t"
        "ret"
    );
}

__attribute__((naked, noreturn)) void longjmp(jmp_buf env, int val) {
    __asm__ volatile (
        "movq %rsi, %rax\n\t"
        "testq %rax, %rax\n\t"
        "jnz 1f\n\t"
        "movq $1, %rax\n\t"
        "1:\n\t"
        "movq (%rdi), %rbx\n\t"
        "movq 8(%rdi), %rbp\n\t"
        "movq 16(%rdi), %r12\n\t"
        "movq 24(%rdi), %r13\n\t"
        "movq 32(%rdi), %r14\n\t"
        "movq 40(%rdi), %r15\n\t"
        "movq 48(%rdi), %rsp\n\t"
        "jmp *56(%rdi)"
    );
}
