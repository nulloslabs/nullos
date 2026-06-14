#include <freestanding/errno.h>
#include <freestanding/sys/syscall.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impl.h>
#include <main/scheduler.h>
#include <io/terminal.h>
#include <main/msr.h>

typedef void (*syscall_fn_t)(syscall_frame_t *);

static syscall_fn_t syscall_table[] = {
    [SYS_read]         = sys_read,
    [SYS_write]        = sys_write,
    [SYS_open]         = sys_open,
    [SYS_close]        = sys_close,
    [SYS_stat]         = sys_stat,
    [SYS_fstat]        = sys_fstat,
    [SYS_lseek]        = sys_lseek,
    [SYS_mmap]         = sys_mmap,
    [SYS_mprotect]     = sys_mprotect,
    [SYS_munmap]       = sys_munmap,
    [SYS_brk]          = sys_brk,
    [SYS_ioctl]        = sys_ioctl,
    [SYS_fcntl]        = sys_fcntl,
    [SYS_pipe]         = sys_pipe,
    [SYS_dup]          = sys_dup,
    [SYS_dup2]         = sys_dup2,
    [SYS_nanosleep]    = sys_nanosleep,
    [SYS_gettimeofday] = sys_gettimeofday,
    [SYS_settimeofday] = sys_settimeofday,
    [SYS_getrlimit]    = sys_getrlimit,
    [SYS_setrlimit]    = sys_setrlimit,
    [SYS_getrusage]    = sys_getrusage,
    [SYS_times]        = sys_times,
    [SYS_getpid]       = sys_getpid,
    [SYS_socket]       = sys_socket,
    [SYS_connect]      = sys_connect,
    [SYS_accept]       = sys_accept,
    [SYS_sendto]       = sys_sendto,
    [SYS_recvfrom]     = sys_recvfrom,
    [SYS_shutdown]     = sys_shutdown,
    [SYS_bind]         = sys_bind,
    [SYS_listen]       = sys_listen,
    [SYS_socketpair]   = sys_socketpair,
    [SYS_reboot]       = sys_reboot,
    [SYS_fork]         = sys_fork,
    [SYS_execve]       = sys_execve,
    [SYS_exit]         = sys_exit,
    [SYS_wait4]        = sys_wait4,
    [SYS_kill]         = sys_kill,
    [SYS_uname]        = sys_uname,
    [SYS_flock]        = sys_flock,
    [SYS_getdents]     = sys_getdents,
    [SYS_getcwd]       = sys_getcwd,
    [SYS_chdir]        = sys_chdir,
    [SYS_mkdir]        = sys_mkdir,
    [SYS_rmdir]        = sys_rmdir,
    [SYS_unlink]       = sys_unlink,
    [SYS_symlink]      = sys_symlink,
    [SYS_chmod]        = sys_chmod,
    [SYS_fchmod]       = sys_fchmod,
    [SYS_getuid]       = sys_getuid,
    [SYS_getgid]       = sys_getgid,
    [SYS_setuid]       = sys_setuid,
    [SYS_setgid]       = sys_setgid,
    [SYS_geteuid]      = sys_geteuid,
    [SYS_getegid]      = sys_getegid,
    [SYS_getppid]      = sys_getppid,
    [SYS_seteuid]      = sys_seteuid,
    [SYS_setegid]      = sys_setegid,
    [SYS_utime]        = sys_utime,
    [SYS_arch_prctl]   = sys_arch_prctl,
    [SYS_mount]        = sys_mount,
    [SYS_umount]       = sys_umount,
    [SYS_sethostname]  = sys_sethostname,
    [SYS_gethostname]  = sys_gethostname,
    [SYS_openat]       = sys_openat,
    [SYS_unlinkat]     = sys_unlinkat,
    [SYS_symlinkat]    = sys_symlinkat,
    [SYS_fchmodat]     = sys_fchmodat,
    [SYS_getsockopt]   = sys_getsockopt,
    [SYS_setsockopt]   = sys_setsockopt,
    [SYS_rt_sigaction] = sys_rt_sigaction,
    [SYS_rt_sigreturn] = sys_rt_sigreturn,
    [SYS_futex]        = sys_futex,
};

extern void syscall_entry(void);

extern void check_signals(syscall_frame_t *frame);

void syscall_dispatch(syscall_frame_t *frame) {
    if (frame->rax < (sizeof(syscall_table) / sizeof(syscall_table[0])) && syscall_table[frame->rax]) syscall_table[frame->rax](frame);
    else frame->rax = (uint64_t)-ENOSYS;
    check_signals(frame);
}

void init_syscalls(void) {
    // Enable syscall/sysret in EFER
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);

    write_msr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_SFMASK, (1 << 9));

    write_msr(MSR_KERNEL_GS_BASE, (uint64_t)current_task_ptr);

    printf("syscalls: initialized syscalls\n");
}
