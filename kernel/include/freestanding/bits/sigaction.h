#pragma once

#include <asm-generic/signal.h>
#include <stddef.h>

#define sa_handler   __sigaction_handler.__sa_handler
#define sa_sigaction __sigaction_handler.__sa_sigaction

struct sigaction {
    union {
        __sighandler_t __sa_handler;
        void (*__sa_sigaction)(int, siginfo_t *, void *);
    } __sigaction_handler;
    sigset_t       sa_mask;
    int            sa_flags;
    __sigrestore_t sa_restorer;
};
