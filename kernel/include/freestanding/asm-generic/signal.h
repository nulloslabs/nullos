#pragma once
#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/asm-generic/signal-defs.h>

#define _NSIG 64
#define _NSIG_BPW 64

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED 31
#define SIGRTMIN  32
#define SIGRTMAX  _NSIG

#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

#define _SIGSET_NWORDS (1024 / (8 * sizeof(unsigned long)))

typedef struct { unsigned long __val[_SIGSET_NWORDS]; } sigset_t;

typedef uint64_t old_sigset_t;

typedef struct sigaltstack {
    void   *ss_sp;
    int     ss_flags;
    size_t  ss_size;
} stack_t;
