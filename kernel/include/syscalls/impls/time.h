#pragma once

#include <syscalls/syscalls.h>

void sys_nanosleep(syscall_frame_t *frame);
void sys_clock_nanosleep(syscall_frame_t *frame);
void sys_getitimer(syscall_frame_t *frame);
void sys_setitimer(syscall_frame_t *frame);
void sys_time(syscall_frame_t *frame);
void sys_gettimeofday(syscall_frame_t *frame);
void sys_times(syscall_frame_t *frame);
void sys_settimeofday(syscall_frame_t *frame);
void sys_clock_gettime(syscall_frame_t *frame);
void sys_clock_getres(syscall_frame_t *frame);
