#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdarg.h>
#include <freestanding/signal.h>
#include <main/panic.h>
#include <main/halt.h>
#include <main/sched.h>
#include <mm/vmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>
#include <io/terminal.h>

static const char* panic_basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

__attribute__((noreturn)) void panicat(const char* file, const char *msg, ...) {
    cli();

    va_list args;
    va_start(args, msg);

    uint64_t rip = (uint64_t)__builtin_return_address(0);
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

    printf("kernel panic at %s: ", panic_basename(file));

    vprintf(msg, args);
    printf("\n");

    printf("\nregisters:\n");
    printf(" rip: %p\n", rip);
    printf(" rsp: %p\n", rsp);

    va_end(args);
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

        syscall_frame_t signal_frame = {0};
        signal_frame.rax = frame->rax;
        signal_frame.rbx = frame->rbx;
        signal_frame.rcx = frame->rip;
        signal_frame.rdx = frame->rdx;
        signal_frame.rsi = frame->rsi;
        signal_frame.rdi = frame->rdi;
        signal_frame.rsp = frame->rsp;
        signal_frame.rbp = frame->rbp;
        signal_frame.r8  = frame->r8;
        signal_frame.r9  = frame->r9;
        signal_frame.r10 = frame->r10;
        signal_frame.r11 = frame->rflags;
        signal_frame.r12 = frame->r12;
        signal_frame.r13 = frame->r13;
        signal_frame.r14 = frame->r14;
        signal_frame.r15 = frame->r15;

        check_signals(&signal_frame);

        frame->rax = signal_frame.rax;
        frame->rbx = signal_frame.rbx;
        frame->rip = signal_frame.rcx;
        frame->rdx = signal_frame.rdx;
        frame->rsi = signal_frame.rsi;
        frame->rdi = signal_frame.rdi;
        frame->rsp = signal_frame.rsp;
        frame->rbp = signal_frame.rbp;
        frame->r8  = signal_frame.r8;
        frame->r9  = signal_frame.r9;
        frame->r10 = signal_frame.r10;
        frame->rflags = signal_frame.r11;
        frame->r12 = signal_frame.r12;
        frame->r13 = signal_frame.r13;
        frame->r14 = signal_frame.r14;
        frame->r15 = signal_frame.r15;
        return;
    }

    // Kernel fault, panic as before
    cli();
    printf("kernel panic at <unknown>: %s\n", reason);
    printf("\nregisters:\n");
    printf(" rip: %p\n", frame->rip);
    printf(" rsp: %p\n", frame->rsp);
    halt();
}
