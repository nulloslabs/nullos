#pragma once

#include <freestanding/stdint.h>
#include <main/gdt.h>

typedef struct {
    uint32_t lapic_id;
    int current_task;
    void *kernel_stack;
    int active;
} cpu_t;

extern cpu_t cpus[MAX_CPUS];
extern int cpu_count;
extern volatile int ap_ready_count;

void init_mp(void);
cpu_t *get_cpu(void);
int get_cpu_index(void);
