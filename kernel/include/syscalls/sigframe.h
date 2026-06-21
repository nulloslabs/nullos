#pragma once

// ============================================================================
// x86-64 signal frame ABI — matches the Linux/glibc rt_sigframe layout.
//
// This header defines the exact byte layout of the signal frame the kernel
// builds on the user stack and reads back via rt_sigreturn.  It must match
// what glibc/musl/busybox expect, because the libc provides the restorer
// trampoline and (for SA_SIGINFO) reads siginfo/ucontext from these offsets.
//
// The user-visible pieces are:
//   struct sigcontext     — the mcontext (saved GP registers + FP pointer)
//   struct _fpstate       — the FXSAVE-compatible 512-byte FP area
//   struct ucontext       — wraps mcontext, sigmask, altstack info
//   struct rt_sigframe    — { restorer ptr, ucontext, siginfo }
//
// Stack layout at handler entry (RSP points at pretcode):
//
//   [RSP + 0]    pretcode         (restorer address — popped by handler `ret`)
//   [RSP + 8]    struct ucontext  (rdx = &this for SA_SIGINFO)
//   [RSP + 8+sizeof(ucontext)]  struct k_siginfo (rsi = &this for SA_SIGINFO)
//   ... below: fpstate area (uc.uc_mcontext.fpstate points here)
//
// When the restorer does `syscall(SYS_rt_sigreturn)`, RSP = &ucontext
// (pretcode was popped).  So the kernel reads rt_sigframe at RSP - 8.
// ============================================================================

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

// ---------------------------------------------------------------------------
// FXSAVE-compatible floating-point state (first 512 bytes of the XSAVE area).
// When XSAVE is available, the kernel writes the full xsave_area_size()-byte
// area; the first 512 bytes always match this layout.
// ---------------------------------------------------------------------------
struct _fpstate {
    uint16_t cwd;              // x87 FPU control word
    uint16_t swd;              // x87 FPU status word
    uint16_t ftw;              // x87 FPU tag word (abridged, FXSAVE format)
    uint16_t fop;              // x87 FPU opcode
    uint64_t rip;              // x87 FPU instruction pointer
    uint64_t rdp;              // x87 FPU data pointer
    uint32_t mxcsr;            // SSE control/status
    uint32_t mxcsr_mask;       // valid MXCSR bits
    uint32_t st_space[32];     // 8 × 128-bit x87 registers
    uint32_t xmm_space[64];    // 16 × 128-bit XMM registers
    uint32_t reserved2[12];    // padding to 512 bytes
    uint32_t reserved3[12];    // software reserved (fp_sw_reserved)
} __attribute__((packed));

// _Static_assert in C99 is _Static_assert; verify FXSAVE area is 512 bytes.
_Static_assert(sizeof(struct _fpstate) == 512, "FXSAVE area must be 512 bytes");

// ---------------------------------------------------------------------------
// sigcontext — the mcontext / saved general-purpose register set.
// Field order and sizes are dictated by the kernel UAPI and must not change.
// (arch/x86/include/uapi/asm/sigcontext.h, struct sigcontext_64)
//
// Mapping note: in this kernel's syscall_frame_t, the `rcx` field holds the
// user RIP and `r11` holds the user RFLAGS (because the `syscall` instruction
// deposits RIP in RCX and RFLAGS in R11).  So:
//   building sigcontext:  ip   = frame->rcx,  flags = frame->r11,
//                         rsp  = frame->rsp
//   restoring frame:      rcx = sigcontext.ip, r11 = sigcontext.flags,
//                         rsp = sigcontext.rsp
// The sigcontext.cx and sigcontext.r11 fields are vestigial for syscall-
// context delivery (they'll hold the RIP/RFLAGS), which matches how the
// `syscall` ABI loses the true cx/r11 register values anyway.
// ---------------------------------------------------------------------------
struct sigcontext {
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;        // (holds RFLAGS value — see note above)
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
    uint64_t rcx;        // (holds RIP value — see note above)
    uint64_t rsp;        // user RSP at signal time (the "sp" field)
    uint64_t rip;        // user RIP at signal time (the "ip" field)
    uint64_t eflags;     // RFLAGS at signal time (the "flags" field)
    uint16_t cs;
    uint16_t gs;
    uint16_t fs;
    uint16_t ss;
    uint64_t err;
    uint64_t trapno;
    uint64_t oldmask;    // old blocked-signal mask
    uint64_t cr2;
    uint64_t fpstate;    // user pointer to struct _fpstate (0 = no FP state)
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

// ---------------------------------------------------------------------------
// stack_t — alternate signal stack descriptor (24 bytes on x86-64).
// ss_flags: SS_ONSTACK (0x1), SS_DISABLE (0x2), SS_AUTODISARM (0x80000000).
// ---------------------------------------------------------------------------
typedef struct {
    void   *ss_sp;
    int     ss_flags;
    size_t  ss_size;
} k_stack_t;

_Static_assert(sizeof(k_stack_t) == 24, "stack_t must be 24 bytes");

#define SS_ONSTACK     1
#define SS_DISABLE     2
#define SS_AUTODISARM  0x80000000U

// uc_flags bits
#define UC_SIGCONTEXT_SS   0x2   // uses sigaltstack
#define UC_STRICT_RESTORE_SS 0x4

// ---------------------------------------------------------------------------
// ucontext — the context saved/restored across a signal handler.
// ---------------------------------------------------------------------------
struct ucontext {
    uint64_t           uc_flags;
    struct ucontext   *uc_link;
    k_stack_t          uc_stack;
    struct sigcontext  uc_mcontext;
    uint64_t           uc_sigmask;   // sigset_t (8 bytes)
};

_Static_assert(offsetof(struct ucontext, uc_flags)   ==  0, "uc.uc_flags offset");
_Static_assert(offsetof(struct ucontext, uc_link)    ==  8, "uc.uc_link offset");
_Static_assert(offsetof(struct ucontext, uc_stack)   == 16, "uc.uc_stack offset");
_Static_assert(offsetof(struct ucontext, uc_mcontext) == 40, "uc.uc_mcontext offset");
_Static_assert(offsetof(struct ucontext, uc_sigmask) == 296, "uc.uc_sigmask offset");

// ---------------------------------------------------------------------------
// siginfo_t — minimal kernel-built siginfo for SA_SIGINFO handlers.
// (Full siginfo is 128 bytes, but we only populate the first 3 fields.)
// ---------------------------------------------------------------------------
struct k_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad;
    uint64_t _pad2[14];   // pad to 128 bytes for ABI compatibility
};

_Static_assert(sizeof(struct k_siginfo) == 128, "siginfo must be 128 bytes");

// ---------------------------------------------------------------------------
// rt_sigframe — the full signal frame placed on the user stack.
// ---------------------------------------------------------------------------
struct rt_sigframe {
    uint64_t          pretcode;     // restorer address (handler `ret` target)
    struct ucontext   uc;           // rdx = &uc for SA_SIGINFO
    struct k_siginfo  info;         // rsi = &info for SA_SIGINFO
};

// ---------------------------------------------------------------------------
// Minimum alternate stack sizes (for sigaltstack validation).
// MINSIGSTKSZ must be large enough for one rt_sigframe + FP state + slack.
// ---------------------------------------------------------------------------
#define MINSIGSTKSZ 2048
#define SIGSTKSZ     8192
