#pragma once

#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>
#include <main/fd.h>
#include <mm/vmm.h>
#include <syscalls/syscalls.h>

#define MAX_TASKS 64

typedef struct {
    pid_t pid;
    pid_t parent_pid;
    int state;
    int priority;
    uint8_t ring;
    uint64_t rsp;
    void *stack_base;
    void *kernel_stack;
    vmm_context_t *ctx;
    uint64_t brk_start;
    uint64_t brk;
    fd_table_t fd_table;
    char cwd[256];
    int exit_status;
} __attribute__((aligned(16))) task_t;

#define TASK_DEAD 0
#define TASK_READY 1
#define TASK_RUNNING 2
#define TASK_ZOMBIE 3

extern task_t tasks[MAX_TASKS];
extern int current_task;
extern task_t* current_task_ptr;
extern volatile int sched_lock;

void init_scheduler(void);
pid_t create_task(void (*entry)(void), uint8_t ring, vmm_context_t *ctx, uint64_t initial_rsp);
pid_t clone_task(syscall_frame_t *frame, vmm_context_t *child_ctx);
void schedule(void);
void exit_task(int status);
