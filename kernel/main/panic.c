#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/signal.h>
#include <io/terminal.h>
#include <main/panic.h>
#include <main/halt.h>
#include <main/scheduler.h>
#include <mm/vmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>

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

// Map CPU exception vectors to signal numbers
static int exception_to_signal(int vector) {
    switch (vector) {
        case 0:  return SIGFPE;   // Division error
        case 4:  return SIGFPE;   // Overflow
        case 5:  return SIGFPE;   // Bounds
        case 6:  return SIGILL;   // Invalid opcode
        case 7:  return SIGFPE;   // Device not available (x87)
        case 8:  return SIGSEGV;  // Double fault
        case 10: return SIGSEGV;  // Invalid TSS
        case 11: return SIGBUS;   // Segment not present
        case 12: return SIGSEGV;  // Stack segment fault
        case 13: return SIGSEGV;  // General protection fault
        case 14: return SIGSEGV;  // Page fault
        case 16: return SIGFPE;   // x87 FP exception
        case 17: return SIGBUS;   // Alignment check
        case 18: return SIGBUS;   // Machine check
        case 19: return SIGFPE;   // SIMD FP exception
        case 30: return SIGSEGV;  // Security exception
        default: return SIGSEGV;
    }
}


void exception_panic(exception_frame_t *frame) {
    const char *reason = "";

    switch (frame->vector) {
        case 0: reason = "a division error occurred"; break;
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
    if ((frame->cs & 3) != 0) {
        int sig = exception_to_signal(frame->vector);

        current_task_ptr->pending_signals |= (1ULL << sig);

        syscall_frame_t sf = {0};
        sf.rax = frame->rax;
        sf.rbx = frame->rbx;
        sf.rcx = frame->rip;
        sf.rdx = frame->rdx;
        sf.rsi = frame->rsi;
        sf.rdi = frame->rdi;
        sf.rsp = frame->rsp;
        sf.rbp = frame->rbp;
        sf.r8  = frame->r8;
        sf.r9  = frame->r9;
        sf.r10 = frame->r10;
        sf.r11 = frame->rflags;
        sf.r12 = frame->r12;
        sf.r13 = frame->r13;
        sf.r14 = frame->r14;
        sf.r15 = frame->r15;

        check_signals(&sf);

        frame->rax = sf.rax;
        frame->rbx = sf.rbx;
        frame->rip = sf.rcx;
        frame->rdx = sf.rdx;
        frame->rsi = sf.rsi;
        frame->rdi = sf.rdi;
        frame->rsp = sf.rsp;
        frame->rbp = sf.rbp;
        frame->r8  = sf.r8;
        frame->r9  = sf.r9;
        frame->r10 = sf.r10;
        frame->rflags = sf.r11;
        frame->r12 = sf.r12;
        frame->r13 = sf.r13;
        frame->r14 = sf.r14;
        frame->r15 = sf.r15;

        return;
    }

    // Kernel fault, panic as before
    cli();
    printf("kernel panic: %s\n", reason);
    printf("\nregisters:\n");
    printf(" rip: %p\n", frame->rip);
    printf(" rsp: %p\n", frame->rsp);
    halt();
}
