#pragma once

#include <syscalls/syscalls.h>

void sys_read(syscall_frame_t *frame);
void sys_write(syscall_frame_t *frame);
void sys_poll(syscall_frame_t *frame);
void sys_ioctl(syscall_frame_t *frame);
void sys_select(syscall_frame_t *frame);
void sys_dup(syscall_frame_t *frame);
void sys_dup2(syscall_frame_t *frame);
void sys_pselect6(syscall_frame_t *frame);
