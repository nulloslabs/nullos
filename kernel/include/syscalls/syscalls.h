#pragma once

#include <freestanding/stdint.h>

/* Yes, I know this isn't consistent with the userspace sys/syscall.h but thats the LibC, not our problem :shrug: */

// Syscall numbers
#define SYS_exit 0
#define SYS_open 1
#define SYS_openat 2
#define SYS_close 3
#define SYS_read 4
#define SYS_write 5
#define SYS_stat 6
#define SYS_fstat 7
#define SYS_chmod 8
#define SYS_fchmod 9
#define SYS_fchmodat 10
#define SYS_mount 11
#define SYS_umount 12
#define SYS_fork 13
#define SYS_execve 14
#define SYS_chdir 15
#define SYS_ioctl 16
#define SYS_dup 17
#define SYS_dup2 18
#define SYS_mkdir 19
#define SYS_getdents 20
#define SYS_getcwd 21
#define SYS_brk 22
#define SYS_waitpid 23
#define SYS_getpid 24
#define SYS_getppid 25
#define SYS_gethostname 26
#define SYS_sethostname 27
#define SYS_lseek 28
#define SYS_uname 29
#define SYS_reboot 30
#define SYS_getuid 31
#define SYS_getgid 32
#define SYS_geteuid 33
#define SYS_getegid 34
#define SYS_setuid 35
#define SYS_setgid 36
#define SYS_seteuid 37
#define SYS_setegid 38
#define SYS_kill 39
#define SYS_nanosleep 40

// Register frame passed to syscall_dispatch
typedef struct {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} __attribute__((packed)) syscall_frame_t;

void syscall_dispatch(syscall_frame_t *frame);
void init_syscalls(void);
