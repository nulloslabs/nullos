#pragma once

#include <syscalls/syscalls.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>

// Actual implementations
void sys_read(syscall_frame_t *frame);
void sys_write(syscall_frame_t *frame);
void sys_open(syscall_frame_t *frame);
void sys_close(syscall_frame_t *frame);
void sys_stat(syscall_frame_t *frame);
void sys_fstat(syscall_frame_t *frame);
void sys_lseek(syscall_frame_t *frame);
void sys_mmap(syscall_frame_t *frame);
void sys_mprotect(syscall_frame_t *frame);
void sys_munmap(syscall_frame_t *frame);
void sys_brk(syscall_frame_t *frame);
void sys_rt_sigaction(syscall_frame_t *frame);
void sys_rt_sigreturn(syscall_frame_t *frame);
void sys_ioctl(syscall_frame_t *frame);
void sys_fcntl(syscall_frame_t *frame);
void sys_pipe(syscall_frame_t *frame);
void sys_dup(syscall_frame_t *frame);
void sys_dup2(syscall_frame_t *frame);
void sys_nanosleep(syscall_frame_t *frame);
void sys_getpid(syscall_frame_t *frame);
void sys_socket(syscall_frame_t *frame);
void sys_connect(syscall_frame_t *frame);
void sys_accept(syscall_frame_t *frame);
void sys_sendto(syscall_frame_t *frame);
void sys_recvfrom(syscall_frame_t *frame);
void sys_shutdown(syscall_frame_t *frame);
void sys_bind(syscall_frame_t *frame);
void sys_listen(syscall_frame_t *frame);
void sys_socketpair(syscall_frame_t *frame);
void sys_reboot(syscall_frame_t *frame);
void sys_fork(syscall_frame_t *frame);
void sys_execve(syscall_frame_t *frame);
void sys_exit(syscall_frame_t *frame);
void sys_wait4(syscall_frame_t *frame);
void sys_kill(syscall_frame_t *frame);
void sys_uname(syscall_frame_t *frame);
void sys_flock(syscall_frame_t *frame);
void sys_getdents(syscall_frame_t *frame);
void sys_getcwd(syscall_frame_t *frame);
void sys_chdir(syscall_frame_t *frame);
void sys_mkdir(syscall_frame_t *frame);
void sys_rmdir(syscall_frame_t *frame);
void sys_unlink(syscall_frame_t *frame);
void sys_symlink(syscall_frame_t *frame);
void sys_chmod(syscall_frame_t *frame);
void sys_fchmod(syscall_frame_t *frame);
void sys_gettimeofday(syscall_frame_t *frame);
void sys_getrlimit(syscall_frame_t *frame);
void sys_getrusage(syscall_frame_t *frame);
void sys_times(syscall_frame_t *frame);
void sys_getuid(syscall_frame_t *frame);
void sys_getgid(syscall_frame_t *frame);
void sys_setuid(syscall_frame_t *frame);
void sys_setgid(syscall_frame_t *frame);
void sys_geteuid(syscall_frame_t *frame);
void sys_getegid(syscall_frame_t *frame);
void sys_getppid(syscall_frame_t *frame);
void sys_seteuid(syscall_frame_t *frame);
void sys_setegid(syscall_frame_t *frame);
void sys_utime(syscall_frame_t *frame);
void sys_arch_prctl(syscall_frame_t *frame);
void sys_setrlimit(syscall_frame_t *frame);
void sys_settimeofday(syscall_frame_t *frame);
void sys_mount(syscall_frame_t *frame);
void sys_umount(syscall_frame_t *frame);
void sys_sethostname(syscall_frame_t *frame);
void sys_gethostname(syscall_frame_t *frame);
void sys_futex(syscall_frame_t *frame);
void sys_openat(syscall_frame_t *frame);
void sys_unlinkat(syscall_frame_t *frame);
void sys_symlinkat(syscall_frame_t *frame);
void sys_fchmodat(syscall_frame_t *frame);
void sys_getsockopt(syscall_frame_t *frame);
void sys_setsockopt(syscall_frame_t *frame);
void sys_getrandom(syscall_frame_t *frame);

// Some public helpers
int copy_from_user(void *kdest, const void *usrc, size_t size);
int copy_to_user(const void *udest, const void *ksrc, size_t size);
bool is_mounted_under(const char* path, const char* fstype, char* relative_out);
void check_signals(syscall_frame_t *frame);
void futex_check_timeouts(void);
