#pragma once

#include <freestanding/stdint.h>
#include <main/gdt.h>

typedef struct {
    uint32_t lapic_id;
    int current_task;
    void *kernel_stack;
    int active;
} cpu_t;

typedef struct {
    uint32_t lapic_id;
    int cpu_index;
    bool used;
} cpu_index_map_entry_t;

#define CPU_INDEX_MAP_SIZE (MAX_CPUS * 2)

extern cpu_t cpus[MAX_CPUS];
extern int cpu_count;
extern volatile int ap_ready_count;
extern cpu_index_map_entry_t cpu_index_map[CPU_INDEX_MAP_SIZE];

uint32_t cpu_index_hash(uint32_t lapic_id);
void clear_cpu_index_map(void);
void map_cpu_index(uint32_t lapic_id, int cpu_index);
int get_cpu_index(void);
cpu_t *get_cpu(void);
void init_mp(void);
