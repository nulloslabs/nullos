#pragma once

#include <stdint.h>
#include <stddef.h>

#define SYSCALL_RFLAG_FIXED (1ULL << 1)
#define SYSCALL_RFLAG_TF    (1ULL << 8)
#define SYSCALL_RFLAG_IF    (1ULL << 9)
#define SYSCALL_RFLAG_DF    (1ULL << 10)
#define SYSCALL_RFLAG_NT    (1ULL << 14)
#define SYSCALL_RFLAG_RF    (1ULL << 16)
#define SYSCALL_RFLAG_VM    (1ULL << 17)
#define SYSCALL_RFLAG_AC    (1ULL << 18)
#define SYSCALL_RFLAG_IOPL  (3ULL << 12)

typedef struct {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t rflags;
} __attribute__((packed)) syscall_frame_t;

// Stack layout used by IRQ stubs.
typedef struct {
    uint64_t es;
    uint64_t ds;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

_Static_assert(offsetof(syscall_frame_t, rip) == 128, "syscall frame RIP offset");
_Static_assert(offsetof(syscall_frame_t, rflags) == 136, "syscall frame RFLAGS offset");
_Static_assert(sizeof(syscall_frame_t) == 144, "syscall frame size");
_Static_assert(offsetof(interrupt_frame_t, rax) == 128, "interrupt frame RAX offset");
_Static_assert(offsetof(interrupt_frame_t, rip) == 136, "interrupt frame RIP offset");
_Static_assert(offsetof(interrupt_frame_t, cs) == 144, "interrupt frame CS offset");
_Static_assert(offsetof(interrupt_frame_t, rsp) == 160, "interrupt frame RSP offset");

typedef void (*syscall_fn_t)(syscall_frame_t *);

extern const syscall_fn_t syscall_table[];

void syscall_dispatch(syscall_frame_t *frame);
void check_signals_from_interrupt(interrupt_frame_t *frame);
void init_syscalls_for_cpu(void);
void init_syscalls(void);
