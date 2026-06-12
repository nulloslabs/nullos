#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIGHUP 1
#define SIGINT 2
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE  8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

int kill(pid_t pid, int sig);
int raise(int sig);

#ifdef __cplusplus
}
#endif