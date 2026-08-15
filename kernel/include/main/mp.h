#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <main/gdt.h>

struct task;

typedef struct {
    uint32_t lapic_id;
    int task_index;
    struct task *task;
    int idle_task;
    uint64_t minimum_virtual_runtime;
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
