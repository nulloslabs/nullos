#pragma once

#include <freestanding/stdint.h>

// Syscall numbers
#define SYS_exit 0
#define SYS_open 1
#define SYS_close 2
#define SYS_read 3
#define SYS_write 4
#define SYS_mount 5
#define SYS_umount 6
#define SYS_fork 7
#define SYS_execve 8
#define SYS_chdir 9
#define SYS_ioctl 10
#define SYS_mkdir 11
#define SYS_getdents 12
#define SYS_getcwd 13
#define SYS_brk 14
#define SYS_waitpid 15
#define SYS_getpid 16
#define SYS_getppid 17
#define SYS_gethostname 18
#define SYS_sethostname 19

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