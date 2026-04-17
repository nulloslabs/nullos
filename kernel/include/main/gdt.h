#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_DS 0x18
#define GDT_USER_CS 0x20
#define GDT_TSS 0x28

#define MAX_CPUS 64

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

typedef struct {
    uint64_t entries[7];
    struct tss tss;
    uint8_t df_stack[4096];
} __attribute__((aligned(16))) cpu_gdt_t;

extern cpu_gdt_t cpu_gdts[MAX_CPUS];

void set_tss_kernel_stack(void *stack);           // BSP shorthand (cpu 0)
void tss_set_kernel_stack_for_cpu(int cpu_index, void *stack);
void init_gdt(void);                              // BSP: calls init_gdt_for_cpu(0)
void init_gdt_for_cpu(int cpu_index);             // APs: call this on each AP