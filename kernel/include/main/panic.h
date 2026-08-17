#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <main/elf.h>

#define panic(msg, ...) dopanic(__func__, (msg) __VA_OPT__(,) __VA_ARGS__)

typedef struct {
    uint64_t es;
    uint64_t ds;
    uint64_t cr0;
    uint64_t cr2;
    uint64_t cr3;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) exception_frame_t;

typedef struct {
    const uint8_t *image;
    const elf64_shdr_t *symtab;
    const elf64_shdr_t *strtab;
    uint64_t load_bias;
} kernel_symbol_table_t;

bool are_kernel_symbols_available(void);

__attribute__((noreturn)) void dopanic(const char *func, const char *msg, ...);
void exception_panic(exception_frame_t *frame);
