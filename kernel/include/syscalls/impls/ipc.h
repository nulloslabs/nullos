#pragma once

#include <syscalls/syscalls.h>

void sys_pipe(syscall_frame_t *frame);
void sys_socket(syscall_frame_t *frame);
void sys_connect(syscall_frame_t *frame);
void sys_accept(syscall_frame_t *frame);
void sys_sendto(syscall_frame_t *frame);
void sys_recvfrom(syscall_frame_t *frame);
void sys_sendmsg(syscall_frame_t *frame);
void sys_recvmsg(syscall_frame_t *frame);
void sys_shutdown(syscall_frame_t *frame);
void sys_bind(syscall_frame_t *frame);
void sys_listen(syscall_frame_t *frame);
void sys_getsockname(syscall_frame_t *frame);
void sys_getpeername(syscall_frame_t *frame);
void sys_socketpair(syscall_frame_t *frame);
void sys_setsockopt(syscall_frame_t *frame);
void sys_getsockopt(syscall_frame_t *frame);
void sys_futex(syscall_frame_t *frame);
void sys_epoll_create(syscall_frame_t *frame);
void sys_epoll_wait(syscall_frame_t *frame);
void sys_epoll_ctl(syscall_frame_t *frame);
void sys_epoll_pwait(syscall_frame_t *frame);
void sys_epoll_create1(syscall_frame_t *frame);
void sys_pipe2(syscall_frame_t *frame);
void sys_epoll_pwait2(syscall_frame_t *frame);
