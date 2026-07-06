#pragma once

#include <freestanding/asm-generic/signal.h>
#include <freestanding/stddef.h>

struct sigaction {
    union {
        __sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sigaction_handler;
    sigset_t       sa_mask;
    int            sa_flags;
    __sigrestore_t sa_restorer;
};

#define sa_handler   __sigaction_handler.sa_handler
#define sa_sigaction __sigaction_handler.sa_sigaction
