#pragma once

#include <freestanding/stdint.h>

/* Yes, I know this isn't consistent with the userspace sys/syscall.h but thats the LibC, not our problem :shrug: */

// Syscall numbers
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_ioctl 16
#define SYS_fcntl 17
#define SYS_writev 20
#define SYS_pipe 22
#define SYS_dup 32
#define SYS_dup2 33
#define SYS_nanosleep 35
#define SYS_getitimer 36
#define SYS_setitimer 38
#define SYS_getpid 39
#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept 43
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_shutdown 48
#define SYS_bind 49
#define SYS_listen 50
#define SYS_socketpair 53
#define SYS_reboot 55
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_uname 63
#define SYS_flock 73
#define SYS_getdents 78
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_mkdir 83
#define SYS_chmod 90
#define SYS_gettimeofday 96
#define SYS_getrlimit 97
#define SYS_getrusage 98
#define SYS_times 100
#define SYS_fchmod 91
#define SYS_getuid 102
#define SYS_getgid 104
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_getppid 110
#define SYS_seteuid 115
#define SYS_setegid 116
#define SYS_arch_prctl 158
#define SYS_settimeofday 164
#define SYS_mount 165
#define SYS_umount 166
#define SYS_sethostname 170
#define SYS_gethostname 175
#define SYS_utime 132
#define SYS_setrlimit 160
#define SYS_openat 257
#define SYS_fchmodat 268
#define SYS_getsockopt 300
#define SYS_setsockopt 301

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
