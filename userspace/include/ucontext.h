#pragma once

#include <stdint.h>
#include <stddef.h>
#include <signal.h>

#define UC_SIGCONTEXT_SS     0x2
#define UC_STRICT_RESTORE_SS 0x4

struct _fpstate {
    uint16_t cwd;
    uint16_t swd;
    uint16_t ftw;
    uint16_t fop;
    uint64_t rip;
    uint64_t rdp;
    uint32_t mxcsr;
    uint32_t mxcsr_mask;
    uint32_t st_space[32];
    uint32_t xmm_space[64];
    uint32_t reserved2[12];
    uint32_t reserved3[12];
} __attribute__((packed));

_Static_assert(sizeof(struct _fpstate) == 512, "FXSAVE area must be 512 bytes");

struct sigcontext {
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t eflags;
    uint16_t cs;
    uint16_t gs;
    uint16_t fs;
    uint16_t ss;
    uint64_t err;
    uint64_t trapno;
    uint64_t oldmask;
    uint64_t cr2;
    uint64_t fpstate;
    uint64_t reserved1[8];
};

_Static_assert(offsetof(struct sigcontext, r8)   ==   0, "sigcontext.r8 offset");
_Static_assert(offsetof(struct sigcontext, r15)  ==  56, "sigcontext.r15 offset");
_Static_assert(offsetof(struct sigcontext, rdi)  ==  64, "sigcontext.rdi offset");
_Static_assert(offsetof(struct sigcontext, rsp)  == 120, "sigcontext.rsp offset");
_Static_assert(offsetof(struct sigcontext, rip)  == 128, "sigcontext.rip offset");
_Static_assert(offsetof(struct sigcontext, eflags) == 136, "sigcontext.eflags offset");
_Static_assert(offsetof(struct sigcontext, cs)   == 144, "sigcontext.cs offset");
_Static_assert(offsetof(struct sigcontext, fpstate) == 184, "sigcontext.fpstate offset");
_Static_assert(sizeof(struct sigcontext) == 256, "sigcontext must be 256 bytes");

typedef struct ucontext {
    uint64_t                 uc_flags;
    struct ucontext         *uc_link;
    stack_t                  uc_stack;
    struct sigcontext        uc_mcontext;
    sigset_t                 uc_sigmask;
} ucontext_t;

_Static_assert(offsetof(struct ucontext, uc_flags)    ==   0, "uc.uc_flags offset");
_Static_assert(offsetof(struct ucontext, uc_link)     ==   8, "uc.uc_link offset");
_Static_assert(offsetof(struct ucontext, uc_stack)    ==  16, "uc.uc_stack offset");
_Static_assert(offsetof(struct ucontext, uc_mcontext) ==  40, "uc.uc_mcontext offset");
_Static_assert(offsetof(struct ucontext, uc_sigmask)  == 296, "uc.uc_sigmask offset");
