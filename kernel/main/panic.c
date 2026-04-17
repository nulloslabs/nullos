#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <io/terminal.h>
#include <main/panic.h>
#include <main/halt.h>
#include <main/scheduler.h>
#include <mm/vmm.h>

void panic(const char *reason) {
    cli();
    uint64_t rip = (uint64_t)__builtin_return_address(0);
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    printf("Kernel panic: %s\n", reason);
    printf("\nRegisters:\n");
    printf(" RIP: 0x%llX\n", rip);
    printf(" RSP: 0x%llX\n", rsp);
    halt();
}

void exception_panic(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t rsp, uint64_t cs) {
    const char *reason = "";
    switch (vector) {
        case 0: reason = "A diving by 0 instruction occurred."; break;
        case 4: reason = "A signed arithmetic overflow occurred."; break;
        case 5: reason = "An instruction that exceeded the bound range occurred."; break;
        case 6: reason = "A invalid opcode instruction occurred."; break;
        case 7: reason = "An instruction tried to access a device that was not available."; break;
        case 8: reason = "A double fault occurred."; break;
        case 13: reason = "A general protection fault occurred."; break;
        case 14: reason = "A page fault occurred."; break;
        case 30: reason = "A security exception occurred."; break;
        case 512: reason = "A reserved exception was called (how the fuck do you mess up that bad)."; break;
        default: reason = "An unknown exception occurred."; break;
    }

    // Check if the fault came from user mode (Ring 3)
    if ((cs & 3) != 0) {
        printf("Userspace fault (PID %d): %s\n", current_task_ptr->pid, reason);
        printf(" Vector: %llu  Error: 0x%llX\n", vector, error_code);
        printf(" RIP: 0x%llX  RSP: 0x%llX\n", rip, rsp);

        // Dump bytes at faulting RIP to see what the CPU actually executed
        if (current_task_ptr && current_task_ptr->ctx) {
            uint8_t code[16];
            read_vmm(current_task_ptr->ctx, code, rip, 16);
            printf("Code: ");
            for (int i = 0; i < 16; i++) printf("%02x ", code[i]);
            printf("\n");
        }
        if (vector == 14) {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            printf(" CR2: 0x%llX\n", cr2);
        }

        sti();
        exit_task(128 + 11);
        __builtin_unreachable();
    }

    // Kernel fault — panic as before
    cli();
    printf("Kernel panic: %s\n", reason);
    printf("\nRegisters:\n");
    printf(" RIP: 0x%llX\n", rip);
    printf(" RSP: 0x%llX\n", rsp);
    halt();
}