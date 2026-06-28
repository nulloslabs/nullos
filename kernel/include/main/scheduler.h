#pragma once

#include <freestanding/stdint.h>
#include <freestanding/signal.h>
#include <freestanding/ucontext.h>
#include <freestanding/sys/types.h>
#include <main/fd.h>
#include <main/spinlocks.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <syscalls/syscalls.h>

#define MAX_TASKS 64
#define USER_STACK_SIZE (4 * 1024 * 1024)
#define KERNEL_STACK_SIZE 32768
#define TASK_STDIN_BUF_SIZE 256

#define TASK_DEAD 0
#define TASK_READY 1
#define TASK_RUNNING 2
#define TASK_ZOMBIE 3
#define TASK_STOPPED 4

typedef struct {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;                 // session leader pid (0 = no session / inherited)
    int state;
    int priority;
    uint8_t ring;
    uint64_t rsp;
    void *stack_base;
    void *kernel_stack;
    vmm_context_t *ctx;
    uint64_t brk_start;
    uint64_t brk;
    uid_t uid;
    uid_t euid;
    gid_t gid;
    gid_t egid;
    fd_table_t fd_table;
    pid_t waiting_for;
    char cwd[256];
    char exe[256];          // path of the executable backing this task (for /proc/<pid>/exe)
    vma_table_t vmas;       // tracked virtual memory areas (for /proc/<pid>/maps)
    int exit_status;
    int term_sig;              // signal that terminated us (0 = normal exit)
    uint64_t fs_base;
    uint64_t gs_base;
    int ctty_idx;
    char stdin_buf[TASK_STDIN_BUF_SIZE];
    int stdin_buf_len;
    int stdin_buf_pos;
    uint64_t sigactions[32 * 4];
    uint64_t pending_signals;
    uint64_t blocked_signals;
    int *clear_child_tid;
    int stop_reported;
    uint64_t orig_rax;
    void *fpu_area;
    void *sas_ss_sp;
    size_t sas_ss_size;
    int sas_ss_flags;
    bool on_altstack;
} __attribute__((aligned(16))) task_t;

extern task_t tasks[MAX_TASKS];
extern int current_task;
extern task_t* current_task_ptr;
extern spinlock_t sched_lock;

pid_t create_task(void (*entry)(void), uint8_t ring, vmm_context_t *ctx, uint64_t initial_rsp);
pid_t clone_task(syscall_frame_t *frame, vmm_context_t *child_ctx);
void schedule(void);
void exit_task(int status);
const vma_table_t *task_vma_table(int pid_idx);
bool signal_pending(void);
void init_scheduler(void);
