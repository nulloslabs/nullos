#include <syscalls/syscalls.h>
#include <syscalls/syscall_impl.h>
#include <main/scheduler.h>
#include <io/terminal.h>
#include <main/errno.h>

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void syscall_entry(void);

void syscall_dispatch(syscall_frame_t *frame) {
    switch (frame->rax) {
        case SYS_exit: sys_exit(frame); break;
        case SYS_open: sys_open(frame); break;
        case SYS_close: sys_close(frame); break;
        case SYS_read: sys_read(frame); break;
        case SYS_write: sys_write(frame); break;
        case SYS_mount: sys_mount(frame); break;
        case SYS_umount: sys_umount(frame); break;
        case SYS_fork: sys_fork(frame); break;
        case SYS_execve: sys_execve(frame); break;
        case SYS_chdir: sys_chdir(frame); break;
        case SYS_ioctl: sys_ioctl(frame); break;
        case SYS_mkdir: sys_mkdir(frame); break;
        case SYS_getdents: sys_getdents(frame); break;
        case SYS_getcwd: sys_getcwd(frame); break;
        case SYS_brk: sys_brk(frame); break;
        case SYS_waitpid: sys_waitpid(frame); break;
        case SYS_getpid: sys_getpid(frame); break;
        case SYS_getppid: sys_getppid(frame); break;
        case SYS_gethostname: sys_gethostname(frame); break;
        case SYS_sethostname: sys_sethostname(frame); break;
        default:
            frame->rax = (uint64_t)-ENOSYS;
            break;
    }
}

void init_syscalls(void) {
    // Enable syscall/sysret in EFER
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);

    // STAR 63:48 is (User CS - 16). SYSRET will load CS from this+16 (0x20) and SS from this+8 (0x18).
    // STAR 47:32 is Kernel CS. SYSCALL will load CS from this (0x08) and SS from this+8 (0x10).
    write_msr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));

    // LSTAR: syscall entry point
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    // SFMASK: mask IF (interrupt flag) on syscall entry
    write_msr(MSR_SFMASK, (1 << 9));

    write_msr(MSR_KERNEL_GS_BASE, (uint64_t)current_task_ptr);

    printf("Syscalls: Initialized syscalls.\n");
}
