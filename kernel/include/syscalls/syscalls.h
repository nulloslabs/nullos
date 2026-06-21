#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

#define USER_ADDR_MAX 0x0000800000000000ULL

#define user_addr_ok(addr, size) ((uint64_t)(addr) < USER_ADDR_MAX && (uint64_t)(size) <= USER_ADDR_MAX - (uint64_t)(addr))
#define user_ptr_ok(ptr, size) user_addr_ok((uint64_t)(ptr), (uint64_t)(size))

// Register frame passed to syscall_dispatch
typedef struct {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} __attribute__((packed)) syscall_frame_t;

void syscall_dispatch(syscall_frame_t *frame);
void init_syscalls(void);
