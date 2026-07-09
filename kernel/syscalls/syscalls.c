#include <freestanding/errno.h>
#include <freestanding/asm/unistd.h>
#include <main/sched.h>
#include <main/msr.h>
#include <io/terminal.h>
#include <io/ttys.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>

const syscall_fn_t syscall_table[] = {
    [__NR_read]            = sys_read,
    [__NR_write]           = sys_write,
    [__NR_open]            = sys_open,
    [__NR_close]           = sys_close,
    [__NR_stat]            = sys_stat,
    [__NR_fstat]           = sys_fstat,
    [__NR_lstat]           = sys_lstat,
    [__NR_poll]            = sys_poll,
    [__NR_lseek]           = sys_lseek,
    [__NR_mmap]            = sys_mmap,
    [__NR_mprotect]        = sys_mprotect,
    [__NR_munmap]          = sys_munmap,
    [__NR_brk]             = sys_brk,
    [__NR_rt_sigaction]    = sys_rt_sigaction,
    [__NR_rt_sigprocmask]  = sys_rt_sigprocmask,
    [__NR_rt_sigreturn]    = sys_rt_sigreturn,
    [__NR_rt_sigtimedwait] = sys_rt_sigtimedwait,
    [__NR_ioctl]           = sys_ioctl,
    [__NR_pread64]         = sys_pread64,
    [__NR_readv]           = sys_readv,
    [__NR_writev]          = sys_writev,
    [__NR_access]          = sys_access,
    [__NR_pipe]            = sys_pipe,
    [__NR_select]          = sys_select,
    [__NR_dup]             = sys_dup,
    [__NR_dup2]            = sys_dup2,
    [__NR_nanosleep]       = sys_nanosleep,
    [__NR_getpid]          = sys_getpid,
    [__NR_sendfile]        = sys_sendfile,
    [__NR_socket]          = sys_socket,
    [__NR_connect]         = sys_connect,
    [__NR_accept]          = sys_accept,
    [__NR_sendto]          = sys_sendto,
    [__NR_recvfrom]        = sys_recvfrom,
    [__NR_shutdown]        = sys_shutdown,
    [__NR_bind]            = sys_bind,
    [__NR_listen]          = sys_listen,
    [__NR_socketpair]      = sys_socketpair,
    [__NR_clone]           = sys_clone,
    [__NR_fork]            = sys_fork,
    [__NR_vfork]           = sys_vfork,
    [__NR_execve]          = sys_execve,
    [__NR_exit]            = sys_exit,
    [__NR_wait4]           = sys_wait4,
    [__NR_kill]            = sys_kill,
    [__NR_uname]           = sys_uname,
    [__NR_fcntl]           = sys_fcntl,
    [__NR_flock]           = sys_flock,
    [__NR_getdents]        = sys_getdents,
    [__NR_getcwd]          = sys_getcwd,
    [__NR_chdir]           = sys_chdir,
    [__NR_rename]          = sys_rename,
    [__NR_mkdir]           = sys_mkdir,
    [__NR_rmdir]           = sys_rmdir,
    [__NR_link]            = sys_link,
    [__NR_unlink]          = sys_unlink,
    [__NR_symlink]         = sys_symlink,
    [__NR_readlink]        = sys_readlink,
    [__NR_chmod]           = sys_chmod,
    [__NR_fchmod]          = sys_fchmod,
    [__NR_umask]           = sys_umask,
    [__NR_gettimeofday]    = sys_gettimeofday,
    [__NR_getrlimit]       = sys_getrlimit,
    [__NR_getrusage]       = sys_getrusage,
    [__NR_times]           = sys_times,
    [__NR_getuid]          = sys_getuid,
    [__NR_getgid]          = sys_getgid,
    [__NR_setuid]          = sys_setuid,
    [__NR_setgid]          = sys_setgid,
    [__NR_geteuid]         = sys_geteuid,
    [__NR_getegid]         = sys_getegid,
    [__NR_setpgid]         = sys_setpgid,
    [__NR_getppid]         = sys_getppid,
    [__NR_getpgrp]         = sys_getpgrp,
    [__NR_setsid]          = sys_setsid,
    [__NR_seteuid]         = sys_seteuid,
    [__NR_setegid]         = sys_setegid,
    [__NR_setresuid]       = sys_setresuid,
    [__NR_getresuid]       = sys_getresuid,
    [__NR_setresgid]       = sys_setresgid,
    [__NR_getresgid]       = sys_getresgid,
    [__NR_getpgid]         = sys_getpgid,
    [__NR_getsid]          = sys_getsid,
    [__NR_utime]           = sys_utime,
    [__NR_arch_prctl]      = sys_arch_prctl,
    [__NR_setrlimit]       = sys_setrlimit,
    [__NR_settimeofday]    = sys_settimeofday,
    [__NR_mount]           = sys_mount,
    [__NR_umount]          = sys_umount,
    [__NR_reboot]          = sys_reboot,
    [__NR_sethostname]     = sys_sethostname,
    [__NR_setdomainname]   = sys_setdomainname,
    [__NR_gettid]          = sys_gettid,
    [__NR_clock_gettime]   = sys_clock_gettime,
    [__NR_tkill]           = sys_tkill,
    [__NR_futex]           = sys_futex,
    [__NR_sched_getaffinity] = sys_sched_getaffinity,
    [__NR_getdents64]      = sys_getdents64,
    [__NR_set_tid_address] = sys_set_tid_address,
    [__NR_exit_group]      = sys_exit_group,
    [__NR_tgkill]          = sys_tgkill,
    [__NR_openat]          = sys_openat,
    [__NR_fstatat]         = sys_fstatat,
    [__NR_unlinkat]        = sys_unlinkat,
    [__NR_symlinkat]       = sys_symlinkat,
    [__NR_readlinkat]      = sys_readlinkat,
    [__NR_fchmodat]        = sys_fchmodat,
    [__NR_pselect6]        = sys_pselect6,
    [__NR_set_robust_list] = sys_set_robust_list,
    [__NR_get_robust_list] = sys_get_robust_list,
    [__NR_utimensat]       = sys_utimensat,
    [__NR_pipe2]           = sys_pipe2,
    [__NR_getsockopt]      = sys_getsockopt,
    [__NR_setsockopt]      = sys_setsockopt,
    [__NR_prlimit64]       = sys_prlimit64,
    [__NR_getrandom]       = sys_getrandom,
    [__NR_rseq]            = sys_rseq,
};

extern void syscall_entry(void);

void syscall_dispatch(syscall_frame_t *frame) {
    current_task_ptr->orig_rax = frame->rax;

    // Check for pending signals on ENTRY too, not just on exit. Without this,
    // a signal pended while the task was preempted by the timer (which doesn't
    // call check_signals) would only be noticed after the syscall completes —
    // adding up to one full syscall's latency (e.g. a large write() to the
    // terminal) before SIGINT/SIGTERM/etc. are acted upon. Checking on entry
    // means a Ctrl+C that arrives during a timer-preempt is handled before
    // the next syscall even starts.
    //
    // If check_signals set up a custom handler (it rewrote frame->rcx to the
    // handler address) or terminated/stopped the task (it doesn't return),
    // we must NOT proceed with the syscall — the frame now describes the
    // signal handler trampoline, not the original syscall.
    uint64_t saved_rcx = frame->rcx;
    check_signals(frame);
    if (frame->rcx != saved_rcx) {
        // A signal handler was installed; frame is now set up for sysret to
        // the handler. Skip the syscall entirely.
        return;
    }

    if (frame->rax < (sizeof(syscall_table) / sizeof(syscall_table[0])) && syscall_table[frame->rax]) {
        syscall_table[frame->rax](frame);
    } else {
        frame->rax = (uint64_t)-ENOSYS;
    }
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
