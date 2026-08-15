#pragma once

#include <mm/kstack.h>

#ifndef __ASSEMBLY__
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <ucontext.h>
#include <linux/rseq.h>
#include <sys/types.h>
#include <main/fd.h>
#include <main/spinlocks.h>
#include <main/mp.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <syscalls/syscalls.h>
#endif

#define MAX_TASKS 1024
#define PID_MAX 32768
#define USER_STACK_SIZE (1 * 1024 * 1024)
#define TASK_STDIN_BUF_SIZE 256
#define LOAD_FIXED_1 2048UL
#define LOAD_EXP_1 1884UL
#define LOAD_EXP_5 2014UL
#define LOAD_EXP_15 2037UL
#define LOAD_UPDATE_US 5000000ULL

#define TASK_DEAD 0
#define TASK_READY 1
#define TASK_RUNNING 2
#define TASK_ZOMBIE 3
#define TASK_STOPPED 4
#define TASK_REAPED 5
#define TASK_SLEEPING 6

#define TASK_KERNEL_STACK_OFFSET 48
#define TASK_SYSCALL_USER_RSP_OFFSET 56

#ifndef __ASSEMBLY__
// TODO (maybe): Make this shit of a struct less messier
typedef struct task {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
    int state;
    int nice;
    uint8_t ring;
    uint64_t rsp;
    void *stack_base;
    void *kernel_stack;
    uint64_t syscall_user_rsp;
    vmm_context_t *ctx;
    uint64_t brk_start;
    uint64_t brk;
    uid_t uid;
    uid_t euid;
    uid_t fsuid;
    gid_t gid;
    gid_t egid;
    gid_t fsgid;
    mode_t umask;
    fd_table_t fd_table;
    pid_t waiting_for;
    char cwd[256];
    char exe[256];
    char name[16];
    uint64_t auxv_blob[16];
    int auxv_blob_words;
    int exit_status;
    int term_sig;
    uint64_t fs_base;
    uint64_t gs_base;
    uint32_t weight;
    uint64_t virtual_runtime;
    uint64_t execution_start_us;
    uint64_t sleep_deadline_us;
    int running_cpu;
    int ctty_idx;
    char stdin_buf[TASK_STDIN_BUF_SIZE];
    int stdin_buf_len;
    int stdin_buf_pos;
    uint64_t sigactions[32 * 4];
    uint64_t pending_signals;
    uint64_t blocked_signals;
    uint64_t real_timer_deadline_us;
    uint64_t real_timer_interval_us;
    struct rseq *rseq;
    uint32_t rseq_sig;
    size_t rseq_len;
    int stop_reported;
    int stopped_by_signal;
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
} task_t;

extern task_t *tasks[MAX_TASKS];
extern spinlock_t sched_lock;

#define current_task (get_cpu()->task_index)
#define current_task_ptr (get_cpu()->task)

bool is_sched_ready(void);
pid_t create_task(void (*entry)(void), uint8_t ring, vmm_context_t *ctx, uint64_t initial_rsp);
pid_t clone_task(syscall_frame_t *frame, vmm_context_t *child_ctx);
pid_t clone_task_flags(syscall_frame_t *frame, vmm_context_t *child_ctx, uint64_t clone_flags, uint64_t new_stack, int *parent_tidptr, int *child_tidptr, uint64_t new_tls);
void schedule(void);
task_t *get_current_task_ptr(void);
void prepare_scheduler_cpu(int cpu_index);
void sleep_current_task_for(uint64_t duration_us);
int get_task_nice(task_t *task);
int set_task_nice(task_t *task, int nice);
void exit_task(int status);
const vma_table_t *task_vma_table(int pid_idx);
task_t *task_by_pid(pid_t pid);
int task_index_by_pid(pid_t pid);
void release_task_slot(int task_idx);
bool signal_pending(void);
void update_interval_timers(void);
uint64_t get_idle_time_us(void);
uint64_t get_context_switch_count(void);
uint64_t get_processes_created(void);
uint64_t get_timer_interrupt_count(void);
uint32_t get_runnable_task_count(void);
pid_t get_last_created_pid(void);
void record_timer_interrupt(void);
uint16_t get_process_count(void);
void get_load_averages(unsigned long loads[3]);
void init_sched(void);
#endif
