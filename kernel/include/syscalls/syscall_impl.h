#pragma once

#include <syscalls/syscalls.h>
#include <freestanding/stdbool.h>

// Actual implementations
void sys_exit(syscall_frame_t *frame);
void sys_open(syscall_frame_t *frame);
void sys_close(syscall_frame_t *frame);
void sys_read(syscall_frame_t *frame);
void sys_write(syscall_frame_t *frame);
void sys_mount(syscall_frame_t *frame);
void sys_umount(syscall_frame_t *frame);
void sys_fork(syscall_frame_t *frame);
void sys_execve(syscall_frame_t *frame);
void sys_chdir(syscall_frame_t *frame);
void sys_ioctl(syscall_frame_t *frame);
void sys_dup(syscall_frame_t *frame);
void sys_dup2(syscall_frame_t *frame);
void sys_mkdir(syscall_frame_t *frame);
void sys_getdents(syscall_frame_t *frame);
void sys_getcwd(syscall_frame_t *frame);
void sys_brk(syscall_frame_t *frame);
void sys_waitpid(syscall_frame_t *frame);
void sys_getpid(syscall_frame_t *frame);
void sys_getppid(syscall_frame_t *frame);
void sys_gethostname(syscall_frame_t *frame);
void sys_sethostname(syscall_frame_t *frame);
void sys_lseek(syscall_frame_t *frame);
void sys_uname(syscall_frame_t *frame);
void sys_reboot(syscall_frame_t *frame);
void sys_getuid(syscall_frame_t *frame);
void sys_getgid(syscall_frame_t *frame);
void sys_geteuid(syscall_frame_t *frame);
void sys_getegid(syscall_frame_t *frame);
void sys_setuid(syscall_frame_t *frame);
void sys_setgid(syscall_frame_t *frame);
void sys_seteuid(syscall_frame_t *frame);
void sys_setegid(syscall_frame_t *frame);
void sys_kill(syscall_frame_t *frame);

// Some public helpers
bool is_mounted_under(const char* path, const char* fstype, char* relative_out);
