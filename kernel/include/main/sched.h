#pragma once

#include <freestanding/stdint.h>
#include <freestanding/signal.h>
#include <freestanding/ucontext.h>
#include <freestanding/linux/rseq.h>
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

// TODO (maybe): Make this shit of a struct less messier
typedef struct {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
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
    char exe[256];
    char name[16];   // thread/process name (prctl PR_SET_NAME), NUL-terminated, max 16 bytes incl. NUL
    vma_table_t vmas;
    uint64_t auxv_blob[16];
    int auxv_blob_words;
    int exit_status;
    int term_sig;
    uint64_t fs_base;
    uint64_t gs_base;
    int ctty_idx;
    char stdin_buf[TASK_STDIN_BUF_SIZE];
    int stdin_buf_len;
    int stdin_buf_pos;
    uint64_t sigactions[32 * 4];
    uint64_t pending_signals;
    uint64_t blocked_signals;
    // I don't know what any of these things below me does...
    uint64_t orig_rax;
    int *clear_child_tid;
    void *fpu_area;
    void *sas_ss_sp;
    size_t sas_ss_size;
    int sas_ss_flags;
    bool on_altstack;
    void *robust_list_head;
    size_t robust_list_len;
    struct rseq *rseq;
    uint32_t rseq_sig;
    size_t rseq_len;
    int stop_reported;
    int stopped_by_signal;
} task_t;

extern task_t tasks[MAX_TASKS];
extern int current_task;
extern task_t* current_task_ptr;
extern spinlock_t sched_lock;

pid_t create_task(void (*entry)(void), uint8_t ring, vmm_context_t *ctx, uint64_t initial_rsp);
pid_t clone_task(syscall_frame_t *frame, vmm_context_t *child_ctx);
pid_t clone_task_flags(syscall_frame_t *frame, vmm_context_t *child_ctx, uint64_t clone_flags, uint64_t new_stack, int *parent_tidptr, int *child_tidptr, uint64_t new_tls);
void schedule(void);
void exit_task(int status);
const vma_table_t *task_vma_table(int pid_idx);
bool signal_pending(void);
void init_sched(void);
