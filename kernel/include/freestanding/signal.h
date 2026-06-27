#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

#define SIGHUP 1
#define SIGINT 2
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

#define SS_ONSTACK    1
#define SS_DISABLE    2
#define SS_AUTODISARM 0x80000000U

#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

typedef uint64_t sigset_t;

typedef struct {
    void   *ss_sp;
    int     ss_flags;
    size_t  ss_size;
} stack_t;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sigaction_handler;
    int sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

#define sa_handler   __sigaction_handler.sa_handler
#define sa_sigaction __sigaction_handler.sa_sigaction
