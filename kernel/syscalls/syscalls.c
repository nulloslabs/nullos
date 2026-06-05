#include <syscalls/syscalls.h>
#include <syscalls/syscall_impl.h>
#include <main/scheduler.h>
#include <io/terminal.h>
#include <freestanding/errno.h>
#include <main/msr.h>

extern void syscall_entry(void);

void syscall_dispatch(syscall_frame_t *frame) {
    switch (frame->rax) {
        case SYS_read: sys_read(frame); break;
        case SYS_write: sys_write(frame); break;
        case SYS_open: sys_open(frame); break;
        case SYS_close: sys_close(frame); break;
        case SYS_stat: sys_stat(frame); break;
        case SYS_fstat: sys_fstat(frame); break;
        case SYS_lseek: sys_lseek(frame); break;
        case SYS_mmap: sys_mmap(frame); break;
        case SYS_mprotect: sys_mprotect(frame); break;
        case SYS_munmap: sys_munmap(frame); break;
        case SYS_brk: sys_brk(frame); break;
        case SYS_ioctl: sys_ioctl(frame); break;
        case SYS_fcntl: sys_fcntl(frame); break;
        case SYS_pipe: sys_pipe(frame); break;
        case SYS_dup: sys_dup(frame); break;
        case SYS_dup2: sys_dup2(frame); break;
        case SYS_nanosleep: sys_nanosleep(frame); break;
        case SYS_gettimeofday: sys_gettimeofday(frame); break;
        case SYS_settimeofday: sys_settimeofday(frame); break;
        case SYS_getrlimit: sys_getrlimit(frame); break;
        case SYS_setrlimit: sys_setrlimit(frame); break;
        case SYS_getrusage: sys_getrusage(frame); break;
        case SYS_times: sys_times(frame); break;
        case SYS_getpid: sys_getpid(frame); break;
        case SYS_socket: sys_socket(frame); break;
        case SYS_connect: sys_connect(frame); break;
        case SYS_accept: sys_accept(frame); break;
        case SYS_sendto: sys_sendto(frame); break;
        case SYS_recvfrom: sys_recvfrom(frame); break;
        case SYS_shutdown: sys_shutdown(frame); break;
        case SYS_bind: sys_bind(frame); break;
        case SYS_listen: sys_listen(frame); break;
        case SYS_socketpair: sys_socketpair(frame); break;
        case SYS_reboot: sys_reboot(frame); break;
        case SYS_fork: sys_fork(frame); break;
        case SYS_execve: sys_execve(frame); break;
        case SYS_exit: sys_exit(frame); break;
        case SYS_wait4: sys_wait4(frame); break;
        case SYS_kill: sys_kill(frame); break;
        case SYS_uname: sys_uname(frame); break;
        case SYS_flock: sys_flock(frame); break;
        case SYS_getdents: sys_getdents(frame); break;
        case SYS_getcwd: sys_getcwd(frame); break;
        case SYS_chdir: sys_chdir(frame); break;
        case SYS_mkdir: sys_mkdir(frame); break;
        case SYS_chmod: sys_chmod(frame); break;
        case SYS_fchmod: sys_fchmod(frame); break;
        case SYS_getuid: sys_getuid(frame); break;
        case SYS_getgid: sys_getgid(frame); break;
        case SYS_setuid: sys_setuid(frame); break;
        case SYS_setgid: sys_setgid(frame); break;
        case SYS_geteuid: sys_geteuid(frame); break;
        case SYS_getegid: sys_getegid(frame); break;
        case SYS_getppid: sys_getppid(frame); break;
        case SYS_seteuid: sys_seteuid(frame); break;
        case SYS_setegid: sys_setegid(frame); break;
        case SYS_utime: sys_utime(frame); break;
        case SYS_arch_prctl: sys_arch_prctl(frame); break;
        case SYS_mount: sys_mount(frame); break;
        case SYS_umount: sys_umount(frame); break;
        case SYS_sethostname: sys_sethostname(frame); break;
        case SYS_gethostname: sys_gethostname(frame); break;
        case SYS_openat: sys_openat(frame); break;
        case SYS_fchmodat: sys_fchmodat(frame); break;
        case SYS_getsockopt: sys_getsockopt(frame); break;
        case SYS_setsockopt: sys_setsockopt(frame); break;
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

    printf("syscalls: initialized syscalls\n");
}
