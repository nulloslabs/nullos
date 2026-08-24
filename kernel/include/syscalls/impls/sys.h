#pragma once

#include <syscalls/syscalls.h>

void sys_uname(syscall_frame_t *frame);
void sys_getrlimit(syscall_frame_t *frame);
void sys_getrusage(syscall_frame_t *frame);
void sys_sysinfo(syscall_frame_t *frame);
void sys_syslog(syscall_frame_t *frame);
void sys_setrlimit(syscall_frame_t *frame);
void sys_reboot(syscall_frame_t *frame);
void sys_sethostname(syscall_frame_t *frame);
void sys_setdomainname(syscall_frame_t *frame);
void sys_prlimit64(syscall_frame_t *frame);
void sys_getrandom(syscall_frame_t *frame);
