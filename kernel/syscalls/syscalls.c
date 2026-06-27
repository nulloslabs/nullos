#include <freestanding/errno.h>
#include <freestanding/sys/syscall.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>
#include <main/scheduler.h>
#include <io/terminal.h>
#include <io/ttys.h>
#include <main/msr.h>

const syscall_fn_t syscall_table[] = {
    [SYS_read]            = sys_read,
    [SYS_write]           = sys_write,
    [SYS_open]            = sys_open,
    [SYS_close]           = sys_close,
    [SYS_stat]            = sys_stat,
    [SYS_fstat]           = sys_fstat,
    [SYS_lstat]           = sys_lstat,
    [SYS_poll]            = sys_poll,
    [SYS_lseek]           = sys_lseek,
    [SYS_mmap]            = sys_mmap,
    [SYS_mprotect]        = sys_mprotect,
    [SYS_munmap]          = sys_munmap,
    [SYS_brk]             = sys_brk,
    [SYS_rt_sigaction]    = sys_rt_sigaction,
    [SYS_rt_sigprocmask]  = sys_rt_sigprocmask,
    [SYS_rt_sigreturn]    = sys_rt_sigreturn,
    [SYS_ioctl]           = sys_ioctl,
    [SYS_pread64]         = sys_pread64,
    [SYS_readv]           = sys_readv,
    [SYS_writev]          = sys_writev,
    [SYS_pipe]            = sys_pipe,
    [SYS_dup]             = sys_dup,
    [SYS_dup2]            = sys_dup2,
    [SYS_nanosleep]       = sys_nanosleep,
    [SYS_getpid]          = sys_getpid,
    [SYS_sendfile]        = sys_sendfile,
    [SYS_socket]          = sys_socket,
    [SYS_connect]         = sys_connect,
    [SYS_accept]          = sys_accept,
    [SYS_sendto]          = sys_sendto,
    [SYS_recvfrom]        = sys_recvfrom,
    [SYS_shutdown]        = sys_shutdown,
    [SYS_bind]            = sys_bind,
    [SYS_listen]          = sys_listen,
    [SYS_socketpair]      = sys_socketpair,
    [SYS_fork]            = sys_fork,
    [SYS_vfork]           = sys_vfork,
    [SYS_execve]          = sys_execve,
    [SYS_exit]            = sys_exit,
    [SYS_wait4]           = sys_wait4,
    [SYS_kill]            = sys_kill,
    [SYS_uname]           = sys_uname,
    [SYS_fcntl]           = sys_fcntl,
    [SYS_flock]           = sys_flock,
    [SYS_getdents]        = sys_getdents,
    [SYS_getcwd]          = sys_getcwd,
    [SYS_chdir]           = sys_chdir,
    [SYS_rename]          = sys_rename,
    [SYS_mkdir]           = sys_mkdir,
    [SYS_rmdir]           = sys_rmdir,
    [SYS_link]            = sys_link,
    [SYS_unlink]          = sys_unlink,
    [SYS_symlink]         = sys_symlink,
    [SYS_readlink]        = sys_readlink,
    [SYS_chmod]           = sys_chmod,
    [SYS_fchmod]          = sys_fchmod,
    [SYS_umask]           = sys_umask,
    [SYS_gettimeofday]    = sys_gettimeofday,
    [SYS_getrlimit]       = sys_getrlimit,
    [SYS_getrusage]       = sys_getrusage,
    [SYS_times]           = sys_times,
    [SYS_getuid]          = sys_getuid,
    [SYS_getgid]          = sys_getgid,
    [SYS_setuid]          = sys_setuid,
    [SYS_setgid]          = sys_setgid,
    [SYS_geteuid]         = sys_geteuid,
    [SYS_getegid]         = sys_getegid,
    [SYS_setpgid]         = sys_setpgid,
    [SYS_getppid]         = sys_getppid,
    [SYS_setsid]          = sys_setsid,
    [SYS_seteuid]         = sys_seteuid,
    [SYS_setegid]         = sys_setegid,
    [SYS_getpgid]         = sys_getpgid,
    [SYS_getsid]          = sys_getsid,
    [SYS_utime]           = sys_utime,
    [SYS_arch_prctl]      = sys_arch_prctl,
    [SYS_setrlimit]       = sys_setrlimit,
    [SYS_settimeofday]    = sys_settimeofday,
    [SYS_mount]           = sys_mount,
    [SYS_umount]          = sys_umount,
    [SYS_reboot]          = sys_reboot,
    [SYS_sethostname]     = sys_sethostname,
    [SYS_gethostname]     = sys_gethostname,
    [SYS_futex]           = sys_futex,
    [SYS_getdents64]      = sys_getdents64,
    [SYS_set_tid_address] = sys_set_tid_address,
    [SYS_exit_group]      = sys_exit_group,
    [SYS_openat]          = sys_openat,
    [SYS_fstatat]         = sys_fstatat,
    [SYS_unlinkat]        = sys_unlinkat,
    [SYS_symlinkat]       = sys_symlinkat,
    [SYS_fchmodat]        = sys_fchmodat,
    [SYS_utimensat]       = sys_utimensat,
    [SYS_getsockopt]      = sys_getsockopt,
    [SYS_setsockopt]      = sys_setsockopt,
    [SYS_getrandom]       = sys_getrandom,
};

extern void syscall_entry(void);

void syscall_dispatch(syscall_frame_t *frame) {
    tty_process_input_signals();
    if (frame->rax < (sizeof(syscall_table) / sizeof(syscall_table[0])) && syscall_table[frame->rax]) syscall_table[frame->rax](frame);
    else frame->rax = (uint64_t)-ENOSYS;
    check_signals(frame);
}

void init_syscalls_for_cpu(void) {
    // Enable syscall/sysret in EFER
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);

    write_msr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_SFMASK, (1 << 9));

    write_msr(MSR_KERNEL_GS_BASE, (uint64_t)current_task_ptr);
}

void init_syscalls(void) {
    init_syscalls_for_cpu();
    printf("syscalls: initialized syscalls\n");
}
