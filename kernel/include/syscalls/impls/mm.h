#pragma once

#include <syscalls/syscalls.h>

void sys_mmap(syscall_frame_t *frame);
void sys_mprotect(syscall_frame_t *frame);
void sys_munmap(syscall_frame_t *frame);
void sys_brk(syscall_frame_t *frame);
