#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/signal.h>
#include <io/terminal.h>
#include <main/panic.h>
#include <main/halt.h>
#include <main/scheduler.h>
#include <mm/vmm.h>

__attribute__((noreturn)) void panic(const char *reason) {
    cli();

    uint64_t rip = (uint64_t)__builtin_return_address(0);
    uint64_t rsp;

    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    printf("kernel panic: %s\n", reason);
    printf("\nregisters:\n");
    printf(" rip: %p\n", rip);
    printf(" rsp: %p\n", rsp);
    halt();
}

__attribute__((noreturn)) void exception_panic(uint64_t vector, uint64_t rip, uint64_t rsp, uint64_t cs) {
    const char *reason = "";

    switch (vector) {
        case 0: reason = "a division by 0 instruction occurred"; break;
        case 4: reason = "a signed arithmetic overflow occurred"; break;
        case 5: reason = "an instruction that exceeded the bound range occurred"; break;
        case 6: reason = "a invalid opcode instruction occurred"; break;
        case 7: reason = "an instruction tried to access a device that was not available"; break;
        case 8: reason = "a double fault occurred"; break;
        case 13: reason = "a general protection fault occurred"; break;
        case 14: reason = "a page fault occurred"; break;
        case 30: reason = "a security exception occurred"; break;
        case 512: reason = "a reserved exception was called"; break;
        default: reason = "an unknown exception occurred"; break;
    }

    // Check if the fault came from user mode (Ring 3)
    if ((cs & 3) != 0) {
        printf("userspace fault (pid %d): %s\n", current_task_ptr->pid, reason);
        printf("\nregisters:\n");
        printf(" rip: %p\n", rip);
        printf(" rsp: %p\n", rsp);
        sti();
        exit_task(128 + SIGSEGV);
        __builtin_unreachable();
    }

    // Kernel fault — panic as before
    cli();
    printf("kernel panic: %s\n", reason);
    printf("\nregisters:\n");
    printf(" rip: %p\n", rip);
    printf(" rsp: %p\n", rsp);
    halt();
}
