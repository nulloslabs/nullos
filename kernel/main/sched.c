#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <linux/sched.h>
#include <main/log.h>
#include <main/string.h>
#include <main/halt.h>
#include <main/panic.h>
#include <main/gdt.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <main/sse.h>
#include <main/fd.h>
#include <main/msr.h>
#include <main/mp.h>
#include <main/assert.h>
#include <main/timekeeping.h>
#include <io/time.h>
#include <io/usb.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <mm/kstack.h>
#include <syscalls/syscall_impls.h>

_Static_assert(__builtin_offsetof(task_t, kstack) == TASK_KSTACK_OFFSET, "task kernel stack offset changed");
_Static_assert(__builtin_offsetof(task_t, syscall_user_rsp) == TASK_SYSCALL_USER_RSP_OFFSET, "task syscall RSP offset changed");

static pid_t next_pid = 0;
static bool sched_ready = false;
static uint64_t idle_time_us = 0;
static uint64_t last_account_us = 0;
static uint64_t last_load_update_us = 0;
static unsigned long load_averages[3];
static uint64_t context_switch_count = 0;
static uint64_t processes_created = 0;
static uint64_t timer_interrupt_count = 0;
static pid_t last_created_pid = 0;
static void *deferred_kstacks[MAX_CPUS];
static task_t *dead_task;
static spinlock_t task_lock = SPINLOCK_INIT;

spinlock_t sched_lock = SPINLOCK_INIT; // Let's keep this public since other functions use it.
task_t *tasks[MAX_TASKS];

static const uint32_t nice_weights[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548, 7620, 6100, 4904, 3906, 3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423, 335, 272, 215, 172, 137,
    110, 87, 70, 56, 45, 36, 29, 23, 18, 15
};

static void idle_task(void) { idle(); }

static uint32_t get_weight_for_nice(int nice) {
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;
    return nice_weights[nice - NICE_MIN];
}

static void initialize_task_scheduling(task_t *task, task_t *parent) {
    task->nice = parent ? parent->nice : 0;
    task->weight = get_weight_for_nice(task->nice);
    task->virtual_runtime = parent ? parent->virtual_runtime : 0;
    task->execution_start_us = 0;
    task->sleep_deadline_us = 0;
    task->running_cpu = -1;
}

static void wake_sleeping_tasks(uint64_t now) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *task = tasks[i];
        if (task == dead_task || task->state != TASK_SLEEPING || now < task->sleep_deadline_us) continue;
        task->sleep_deadline_us = 0;
        task->state = TASK_READY;
    }
}

static pid_t alloc_pid_locked(void) {
    for (int tries = 0; tries < PID_MAX; tries++) {
        pid_t pid = next_pid++;
        if (next_pid >= PID_MAX) next_pid = 1;
        bool used = false;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i] != dead_task && tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) { used = true; break; }
        }
        if (!used) return pid;
    }
    return -EAGAIN;
}

static bool alloc_task_slot(int i) {
    if (tasks[i] != dead_task) {
        memset(tasks[i], 0, sizeof(*tasks[i]));
        return true;
    }
    task_t *task = malloc(sizeof(*task));
    if (!task) return false;
    memset(task, 0, sizeof(*task));
    tasks[i] = task;
    return true;
}


static unsigned long calc_load(unsigned long load, unsigned long exp, unsigned long active) {
    return (load * exp + active * (LOAD_FIXED_1 - exp)) >> 11;
}

static unsigned long count_runnable_tasks(void) {
    unsigned long count = 0;
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i]->state == TASK_READY || tasks[i]->state == TASK_RUNNING) count++;
    return count;
}

static void update_load_averages(void) {
    uint64_t now = get_monotonic_time_us();
    if (!last_load_update_us) { last_load_update_us = now; return; }
    while (now - last_load_update_us >= LOAD_UPDATE_US) {
        unsigned long active = count_runnable_tasks() * LOAD_FIXED_1;
        load_averages[0] = calc_load(load_averages[0], LOAD_EXP_1, active);
        load_averages[1] = calc_load(load_averages[1], LOAD_EXP_5, active);
        load_averages[2] = calc_load(load_averages[2], LOAD_EXP_15, active);
        last_load_update_us += LOAD_UPDATE_US;
    }
}

task_t *get_current_task_ptr(void) { return current_task_ptr; }
int get_task_nice(task_t *task) { return task ? task->nice : 0; }

void let_current_task_sleep(uint64_t duration_us) {
    assert(current_task_ptr != NULL);
    uint64_t now = get_monotonic_time_us();
    current_task_ptr->sleep_deadline_us = duration_us > UINT64_MAX - now ? UINT64_MAX : now + duration_us;
    current_task_ptr->state = TASK_SLEEPING;
    spin_unlock(&sched_lock);
    yield_sched();
    spin_lock(&sched_lock);
    current_task_ptr->sleep_deadline_us = 0;
}

int set_task_nice(task_t *task, int nice) {
    if (!task) return -ESRCH;
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;
    task->nice = nice;
    task->weight = get_weight_for_nice(nice);
    return 0;
}

task_t *task_by_pid(pid_t pid) {
    int idx = task_index_by_pid(pid);
    return idx < 0 ? NULL : tasks[idx];
}

int task_index_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i] != dead_task && tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) return i;
    return -1;
}

void release_task_slot(int task_idx) {
    if (task_idx < 0 || task_idx >= MAX_TASKS || tasks[task_idx] == dead_task) return;
    assert(tasks[task_idx] != NULL && tasks[task_idx] != dead_task);
    if ((tasks[task_idx]->state == TASK_ZOMBIE || tasks[task_idx]->state == TASK_REAPED) && tasks[task_idx]->running_cpu >= 0) { tasks[task_idx]->state = TASK_REAPED; return; }
    if (tasks[task_idx]->kstack) free_kstack(tasks[task_idx]->kstack);
    if (tasks[task_idx]->fpu_area) vfree(tasks[task_idx]->fpu_area);
    free(tasks[task_idx]);
    tasks[task_idx] = dead_task;
}

uint64_t get_idle_time_us(void) {
    uint64_t idle = idle_time_us;
    uint64_t now = get_monotonic_time_us();
    if (current_task == get_cpu()->idle_task && now >= last_account_us) idle += now - last_account_us;
    return idle;
}

uint64_t get_context_switch_count(void) { return context_switch_count; }
uint64_t get_processes_created(void) { return processes_created; }
uint64_t get_timer_interrupt_count(void) { return __atomic_load_n(&timer_interrupt_count, __ATOMIC_RELAXED); }
pid_t get_last_created_pid(void) { return last_created_pid; }
void record_timer_interrupt(void) { __atomic_add_fetch(&timer_interrupt_count, 1, __ATOMIC_RELAXED); }

uint32_t get_runnable_task_count(void) {
    uint32_t count = 0;
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i]->state == TASK_READY || tasks[i]->state == TASK_RUNNING) count++;
    return count;
}

uint16_t get_process_count(void) {
    uint16_t count = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i]->state != TASK_DEAD) count++;
    return count;
}

void get_load_averages(unsigned long loads[3]) {
    update_load_averages();
    loads[0] = load_averages[0] << 5;
    loads[1] = load_averages[1] << 5;
    loads[2] = load_averages[2] << 5;
}

bool is_sched_ready(void) {
    return sched_ready;
}

const vma_table_t *task_vma_table(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= MAX_TASKS) return NULL;
    if (tasks[pid_idx]->state == TASK_DEAD) return NULL;
    return tasks[pid_idx]->ctx ? &tasks[pid_idx]->ctx->vmas : NULL;
}

pid_t create_task(void (*entry)(void), uint8_t ring, vmm_context_t *ctx, uint64_t initial_rsp) {
    assert(ring == TASK_RING_0 || ring == TASK_RING_3);
    uint64_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) {
            if (!alloc_task_slot(i)) { spin_unlock_irqrestore(&task_lock, flags); return -ENOMEM; }
            uint64_t cs, ss;
            void *stack;
            tasks[i]->ring = ring;

            if (ring == 0) {
                cs = 0x08;
                ss = 0x10;
                stack = alloc_kstack();
                if (!stack) {
                    release_task_slot(i);
                    spin_unlock_irqrestore(&task_lock, flags);
                    return -ENOMEM;
                }
                tasks[i]->ctx = ctx ? ctx : &kernel_context;
                tasks[i]->kstack = stack;
            } else {
                cs = 0x23;
                ss = 0x1B;
                if (initial_rsp) {
                    stack = NULL;
                } else {
                    stack = vmalloc_user_ex(ctx, USER_STACK_SIZE);
                    if (!stack) {
                        release_task_slot(i);
                        spin_unlock_irqrestore(&task_lock, flags);
                        return -ENOMEM;
                    }
                }
                tasks[i]->ctx = ctx;
                tasks[i]->kstack = alloc_kstack();
                if (!tasks[i]->kstack) {
                    release_task_slot(i);
                    spin_unlock_irqrestore(&task_lock, flags);
                    return -ENOMEM;
                }
            }

            tasks[i]->stack_base = stack;
            tasks[i]->ring = ring;
            tasks[i]->ctx = ctx ? ctx : &kernel_context;
            tasks[i]->uid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->uid;
            tasks[i]->euid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->euid;
            tasks[i]->fsuid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->fsuid;
            tasks[i]->gid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->gid;
            tasks[i]->egid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->egid;
            tasks[i]->fsgid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->fsgid;
            tasks[i]->umask = current_task_ptr ? current_task_ptr->umask : 0022;
            tasks[i]->fs_base = 0;
            tasks[i]->gs_base = 0;
            tasks[i]->ctty_idx = current_task_ptr ? current_task_ptr->ctty_idx : 1;

            init_fd_table(&tasks[i]->fd_table);
            strcpy(tasks[i]->cwd, "/");
            tasks[i]->exe[0] = '\0';
            tasks[i]->name[0] = '\0';
            memset(tasks[i]->sigactions, 0, sizeof(tasks[i]->sigactions));
            tasks[i]->pgid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->pgid;
            tasks[i]->sid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->sid;
            tasks[i]->term_sig = 0;
            tasks[i]->stop_reported = 0;
            tasks[i]->stopped_by_signal = 0;
            tasks[i]->pending_signals = 0;
            tasks[i]->blocked_signals = 0;
            tasks[i]->real_timer_deadline_us = 0;
            tasks[i]->real_timer_interval_us = 0;
            tasks[i]->on_altstack = false;
            tasks[i]->sas_ss_sp = NULL;
            tasks[i]->sas_ss_size = 0;
            tasks[i]->sas_ss_flags = 0;
            tasks[i]->robust_list_head = NULL;
            tasks[i]->rseq     = NULL;
            tasks[i]->rseq_len = 0;
            tasks[i]->rseq_sig = 0;

            // Per-task FPU area: freshly initialized to a clean state so the
            // new task starts with sane x87/SSE registers rather than the
            // previous owner's.  Kernel tasks (ring 0) also get one so any
            // FP use inside the kernel doesn't leak across tasks.
            tasks[i]->fpu_area = vmalloc(get_fpu_state_size());
            if (tasks[i]->fpu_area) {
                init_fpu_area(tasks[i]->fpu_area);
            }

            uint64_t v_rsp;
            if (ring == 0) {
                v_rsp = initial_rsp ? initial_rsp : ((uint64_t)stack + KSTACK_SIZE);
            } else {
                v_rsp = initial_rsp ? initial_rsp : ((uint64_t)stack + USER_STACK_SIZE);
            }

            uint64_t k_rsp = (uint64_t)kstack_top(tasks[i]->kstack);

            #define PUSH(val) do { \
                k_rsp -= 8; \
                uint64_t _pv = (uint64_t)(val); \
                (void)write_vmm(&kernel_context, k_rsp, &_pv, 8); \
            } while(0)

            PUSH(0);
            PUSH((uint64_t)exit_task);

            PUSH(ss);
            PUSH(v_rsp);
            PUSH(0x202);
            PUSH(cs);
            PUSH((uint64_t)entry);

            for (int r = 0; r < 15; r++) { PUSH(0); }

            uint64_t data_seg = (ring == 0) ? 0x10 : 0x1B;
            PUSH(data_seg);
            PUSH(data_seg);

            #undef PUSH

            tasks[i]->rsp = k_rsp;
            tasks[i]->pid = alloc_pid_locked();
            tasks[i]->ppid = current_task_ptr ? current_task_ptr->pid : 0;
            tasks[i]->state = TASK_READY;
            initialize_task_scheduling(tasks[i], current_task_ptr);

            processes_created++;
            last_created_pid = tasks[i]->pid;
            spin_unlock_irqrestore(&task_lock, flags);
            return tasks[i]->pid;
        }
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return -EAGAIN;
}

pid_t clone_task(syscall_frame_t *frame, vmm_context_t *child_ctx) {
    uint64_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) {
            if (!alloc_task_slot(i)) { spin_unlock_irqrestore(&task_lock, flags); return -ENOMEM; }
            tasks[i]->stack_base = current_task_ptr->stack_base;
            tasks[i]->ring = current_task_ptr->ring;
            tasks[i]->ctx = child_ctx;
            strcpy(tasks[i]->cwd, current_task_ptr->cwd);
            strcpy(tasks[i]->exe, current_task_ptr->exe);
            strcpy(tasks[i]->name, current_task_ptr->name);
            tasks[i]->ppid = current_task_ptr->pid;
            tasks[i]->pgid = current_task_ptr->pgid;
            tasks[i]->uid = current_task_ptr->uid;
            tasks[i]->euid = current_task_ptr->euid;
            tasks[i]->fsuid = current_task_ptr->fsuid;
            tasks[i]->gid = current_task_ptr->gid;
            tasks[i]->egid = current_task_ptr->egid;
            tasks[i]->fsgid = current_task_ptr->fsgid;
            tasks[i]->umask = current_task_ptr->umask;
            tasks[i]->fs_base = current_task_ptr->fs_base;
            tasks[i]->gs_base = current_task_ptr->gs_base;
            tasks[i]->ctty_idx = current_task_ptr->ctty_idx;

            // A forked child gets a FRESH canonical line buffer.  The task
            // slot may have been reused from a previously-dead task whose
            // stdin_buf_len/pos were non-zero; if we don't reset them here
            // the child's first read() returns stale garbage instead of
            // blocking for real input, which wedges getty/login/bash at the
            // login prompt after a stray boot keypress.
            tasks[i]->stdin_buf_len = 0;
            tasks[i]->stdin_buf_pos = 0;

            // Inherit signal dispositions from parent (POSIX fork semantics)
            memcpy(tasks[i]->sigactions, current_task_ptr->sigactions, sizeof(tasks[i]->sigactions));
            tasks[i]->blocked_signals = current_task_ptr->blocked_signals;
            tasks[i]->pending_signals = 0;
            tasks[i]->real_timer_deadline_us = 0;
            tasks[i]->real_timer_interval_us = 0;
            tasks[i]->sid = current_task_ptr->sid;
            tasks[i]->term_sig = 0;
            tasks[i]->stop_reported = 0;
            tasks[i]->stopped_by_signal = 0;
            tasks[i]->on_altstack = false;
            tasks[i]->sas_ss_sp = current_task_ptr->sas_ss_sp;
            tasks[i]->sas_ss_size = current_task_ptr->sas_ss_size;
            tasks[i]->sas_ss_flags = current_task_ptr->sas_ss_flags;            // Robust list is per-thread and not inherited across fork.
            tasks[i]->robust_list_head = NULL;
            tasks[i]->rseq            = NULL;
            tasks[i]->rseq_len        = 0;
            tasks[i]->rseq_sig        = 0;
            // Per-task FPU area.  The child inherits the parent's current FP
            // state (POSIX fork: child sees a snapshot of the parent's FPU).
            // We save the live state into the child's fresh buffer.
            tasks[i]->fpu_area = vmalloc(get_fpu_state_size());
            if (tasks[i]->fpu_area) {
                init_fpu_area(tasks[i]->fpu_area);
                // Copy the parent's last-saved FPU state into the child.
                if (current_task_ptr->fpu_area) {
                    memcpy(tasks[i]->fpu_area, current_task_ptr->fpu_area, get_fpu_state_size());
                }
            }

            void *kstack = alloc_kstack();
            if (!kstack) {
                release_task_slot(i);
                spin_unlock_irqrestore(&task_lock, flags);
                return -ENOMEM;
            }
            tasks[i]->kstack = kstack;

            memcpy(&tasks[i]->fd_table, &current_task_ptr->fd_table, sizeof(fd_table_t));
            for (int fd = 0; fd < FD_MAX; fd++) {
                if (tasks[i]->fd_table.entries[fd].open) retain_fd_entry(&tasks[i]->fd_table.entries[fd]);
            }

            uint64_t v_rsp = (uint64_t)kstack_top(kstack);

            #define PUSH(val) do { \
                v_rsp -= 8; \
                uint64_t _pv = (uint64_t)(val); \
                (void)write_vmm(&kernel_context, v_rsp, &_pv, 8); \
            } while(0)

            PUSH(0x1B);
            PUSH(frame->rsp);
            PUSH(frame->rflags);
            PUSH(0x23);
            PUSH(frame->rip);

            PUSH(0);
            PUSH(frame->rbx);
            PUSH(frame->rcx);
            PUSH(frame->rdx);
            PUSH(frame->rsi);
            PUSH(frame->rdi);
            PUSH(frame->rbp);
            PUSH(frame->r8);
            PUSH(frame->r9);
            PUSH(frame->r10);
            PUSH(frame->r11);
            PUSH(frame->r12);
            PUSH(frame->r13);
            PUSH(frame->r14);
            PUSH(frame->r15);

            PUSH(0x1B);
            PUSH(0x1B);

            #undef PUSH

            tasks[i]->rsp      = v_rsp;
            tasks[i]->pid      = alloc_pid_locked();
            tasks[i]->state = TASK_READY;
            initialize_task_scheduling(tasks[i], current_task_ptr);

            processes_created++;
            last_created_pid = tasks[i]->pid;
            spin_unlock_irqrestore(&task_lock, flags);
            return tasks[i]->pid;
        }
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return -EAGAIN;
}

pid_t clone_task_flags(syscall_frame_t *frame, vmm_context_t *ctx, uint64_t flags, uint64_t new_rsp, int *parent_tidptr, int *child_tidptr, uint64_t new_fs_base) {
    uint64_t iflags;
    spin_lock_irqsave(&task_lock, &iflags);

    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD) continue;
        if (!alloc_task_slot(i)) { spin_unlock_irqrestore(&task_lock, iflags); return -ENOMEM; }

        tasks[i]->stack_base = current_task_ptr->stack_base;
        tasks[i]->ring       = current_task_ptr->ring;
        tasks[i]->ctx        = ctx;
        strcpy(tasks[i]->cwd, current_task_ptr->cwd);
        strcpy(tasks[i]->exe, current_task_ptr->exe);
        strcpy(tasks[i]->name, current_task_ptr->name);


        // CLONE_PARENT and CLONE_THREAD both make the new task's parent the
        // same as the caller's parent (i.e. the sibling/leader relationship);
        // otherwise the caller is the parent.
        tasks[i]->ppid     = (flags & (CLONE_PARENT | CLONE_THREAD)) ? current_task_ptr->ppid : current_task_ptr->pid;
        // Newly created clone children belong to the same process group as the caller.
        tasks[i]->pgid     = current_task_ptr->pgid;
        tasks[i]->uid      = current_task_ptr->uid;
        tasks[i]->euid     = current_task_ptr->euid;
        tasks[i]->fsuid    = current_task_ptr->fsuid;
        tasks[i]->gid      = current_task_ptr->gid;
        tasks[i]->egid     = current_task_ptr->egid;
        tasks[i]->fsgid    = current_task_ptr->fsgid;
        tasks[i]->umask    = current_task_ptr->umask;
        tasks[i]->ctty_idx = current_task_ptr->ctty_idx;
        tasks[i]->sid      = current_task_ptr->sid;
        // CLONE_SETTLS installs the requested TLS pointer for the child;
        // otherwise it inherits the parent's.
        tasks[i]->fs_base  = (flags & CLONE_SETTLS) ? new_fs_base : current_task_ptr->fs_base;
        tasks[i]->gs_base  = current_task_ptr->gs_base;

        // Fresh canonical line buffer for the child (see clone_task): the
        // task slot may be reused from a dead task with stale stdin_buf
        // state, which would otherwise leak through to the child's reads.
        tasks[i]->stdin_buf_len = 0;
        tasks[i]->stdin_buf_pos = 0;

        memcpy(tasks[i]->sigactions, current_task_ptr->sigactions, sizeof(tasks[i]->sigactions));
        tasks[i]->blocked_signals = current_task_ptr->blocked_signals;
        // CLONE_THREAD shares the pending signal set with the parent, but since
        // our model keeps a per-task bitmap we start clean (the group signal
        // semantic is approximated by kill targeting the thread).
        tasks[i]->pending_signals  = 0;
        tasks[i]->real_timer_deadline_us = 0;
        tasks[i]->real_timer_interval_us = 0;
        tasks[i]->term_sig         = 0;
        tasks[i]->stop_reported    = 0;
        tasks[i]->stopped_by_signal = 0;
        tasks[i]->on_altstack      = false;
        tasks[i]->sas_ss_sp        = current_task_ptr->sas_ss_sp;
        tasks[i]->sas_ss_size      = current_task_ptr->sas_ss_size;
        tasks[i]->sas_ss_flags     = current_task_ptr->sas_ss_flags;
        // Robust list is per-thread and not inherited across fork.
        tasks[i]->robust_list_head = NULL;
        tasks[i]->rseq             = NULL;
        tasks[i]->rseq_len         = 0;
        tasks[i]->rseq_sig         = 0;

        tasks[i]->fpu_area = vmalloc(get_fpu_state_size());
        if (tasks[i]->fpu_area) {
            init_fpu_area(tasks[i]->fpu_area);
            if (current_task_ptr->fpu_area)
                memcpy(tasks[i]->fpu_area, current_task_ptr->fpu_area, get_fpu_state_size());
        }

        void *kstack = alloc_kstack();
        if (!kstack) {
            release_task_slot(i);
            spin_unlock_irqrestore(&task_lock, iflags);
            return -ENOMEM;
        }
        tasks[i]->kstack = kstack;

        // CLONE_FILES shares the file descriptor table.  We do not implement
        // reference-counted shared tables, so we always copy.  This is
        // behaviourally identical to the historical fork() here.
        memcpy(&tasks[i]->fd_table, &current_task_ptr->fd_table, sizeof(fd_table_t));
        for (int fd = 0; fd < FD_MAX; fd++) {
            if (tasks[i]->fd_table.entries[fd].open)
                retain_fd_entry(&tasks[i]->fd_table.entries[fd]);
        }

        // Write the child's pid into the parent's tidptr now (parent can read
        // it as soon as clone returns).
        if ((flags & CLONE_PARENT_SETTID) && parent_tidptr) {
            int pid = 0; // assigned below; rewrite once known
            if (write_vmm(current_task_ptr->ctx, (uint64_t)parent_tidptr, &pid, sizeof(int)) < 0) {
                release_task_slot(i);
                spin_unlock_irqrestore(&task_lock, iflags);
                return -EFAULT;
            }
        }

        uint64_t v_rsp = (uint64_t)kstack_top(kstack);

        #define PUSH(val) do { \
            v_rsp -= 8; \
            uint64_t _pv = (uint64_t)(val); \
            (void)write_vmm(&kernel_context, v_rsp, &_pv, 8); \
        } while(0)

        // iretq frame
        PUSH(0x1B);                           // ss
        PUSH(new_rsp ? new_rsp : frame->rsp); // user rsp
        PUSH(frame->rflags);                  // rflags
        PUSH(0x23);                           // cs
        PUSH(frame->rip);                     // user rip (return point)

        // general purpose registers (rax comes first, == 0 for the child)
        PUSH(0);                              // rax -> child sees clone() == 0
        PUSH(frame->rbx);
        PUSH(frame->rcx);
        PUSH(frame->rdx);
        PUSH(frame->rsi);
        PUSH(frame->rdi);
        PUSH(frame->rbp);
        PUSH(frame->r8);
        PUSH(frame->r9);
        PUSH(frame->r10);
        PUSH(frame->r11);
        PUSH(frame->r12);
        PUSH(frame->r13);
        PUSH(frame->r14);
        PUSH(frame->r15);

        PUSH(0x1B);
        PUSH(0x1B);

        #undef PUSH

        tasks[i]->rsp            = v_rsp;
        tasks[i]->pid            = alloc_pid_locked();
        tasks[i]->state = TASK_READY;
        initialize_task_scheduling(tasks[i], current_task_ptr);
        tasks[i]->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? child_tidptr : NULL;

        // Stamp the real pid into parent_tidptr now that it is known.
        if ((flags & CLONE_PARENT_SETTID) && parent_tidptr) {
            int pid = tasks[i]->pid;
            (void)write_vmm(current_task_ptr->ctx, (uint64_t)parent_tidptr, &pid, sizeof(int));
        }
        // CLONE_CHILD_SETTID: child itself observes its pid at child_tidptr.
        if ((flags & CLONE_CHILD_SETTID) && child_tidptr) {
            int pid = tasks[i]->pid;
            (void)write_vmm(ctx, (uint64_t)child_tidptr, &pid, sizeof(int));
        }

        processes_created++;
        last_created_pid = tasks[i]->pid;
        spin_unlock_irqrestore(&task_lock, iflags);
        return tasks[i]->pid;
    }
    spin_unlock_irqrestore(&task_lock, iflags);
    return -EAGAIN;
}

void update_interval_timers(void) {
    uint64_t now = time_get_realtime_us();

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *task = tasks[i];
        if (task->state == TASK_DEAD || task->state == TASK_ZOMBIE ||
            task->real_timer_deadline_us == 0 || now < task->real_timer_deadline_us) {
            continue;
        }

        task->pending_signals |= (1ULL << SIGALRM);
        if (task->real_timer_interval_us) {
            uint64_t elapsed = now - task->real_timer_deadline_us;
            uint64_t periods = elapsed / task->real_timer_interval_us + 1;
            task->real_timer_deadline_us += periods * task->real_timer_interval_us;
        } else {
            task->real_timer_deadline_us = 0;
        }

        if (task->state == TASK_STOPPED) task->state = TASK_READY;
    }
}

void schedule(void) {
    int cpu_index = get_cpu_index();
    cpu_t *cpu = &cpus[cpu_index];
    if (deferred_kstacks[cpu_index]) {
        free_kstack(deferred_kstacks[cpu_index]);
        deferred_kstacks[cpu_index] = NULL;
    }
    uint64_t now = get_monotonic_time_us();
    if (current_task == cpu->idle_task && now >= last_account_us) idle_time_us += now - last_account_us;
    last_account_us = now;
    update_load_averages();
    update_interval_timers();
    check_futex_timeouts();
    wake_sleeping_tasks(now);

    int old_task = current_task;
    task_t *old = current_task_ptr;

    if (old && old_task != cpu->idle_task) {
        uint64_t elapsed = now >= old->execution_start_us ? now - old->execution_start_us : 0;
        if (elapsed && old->weight) old->virtual_runtime += elapsed * NICE_0_LOAD / old->weight;
        if (old->state == TASK_RUNNING) old->state = TASK_READY;
        old->running_cpu = -1;
    }

    int next = cpu->idle_task;
    uint64_t best_runtime = UINT64_MAX;
    for (int i = 0; i < MAX_TASKS; i++) {
        int candidate_index = (old_task + i + 1) % MAX_TASKS;
        task_t *candidate = tasks[candidate_index];
        if (candidate == dead_task || candidate->state != TASK_READY || candidate->running_cpu >= 0) continue;
        uint64_t floor = cpu->minimum_virtual_runtime > SCHED_WAKEUP_GRANULARITY_US ? cpu->minimum_virtual_runtime - SCHED_WAKEUP_GRANULARITY_US : 0;
        if (candidate->virtual_runtime < floor) candidate->virtual_runtime = floor;
        if (candidate->virtual_runtime < best_runtime) { best_runtime = candidate->virtual_runtime; next = candidate_index; }
    }

    bool exiting = old && (old->state == TASK_ZOMBIE || old->state == TASK_REAPED);
    bool reap_old = exiting && (old->state == TASK_REAPED || old->ppid == 0);
    vmm_context_t *old_ctx = old ? old->ctx : NULL;

    if (next >= 0 && tasks[next] != dead_task) {
        // Eager FPU save of the outgoing task (before its registers are
        // clobbered by the incoming task).  Skip if the outgoing task is a
        // zombie being reaped above — its fpu_area is already gone.
        if (old_task != next && tasks[old_task]->fpu_area && tasks[old_task]->state != TASK_DEAD) {
            save_fpu_state(tasks[old_task]->fpu_area);
        }

        current_task = next;
        tasks[current_task]->state = TASK_RUNNING;
        current_task_ptr = tasks[current_task];
        current_task_ptr->running_cpu = cpu_index;
        current_task_ptr->execution_start_us = now;
        if (next != cpu->idle_task && current_task_ptr->virtual_runtime > cpu->minimum_virtual_runtime) cpu->minimum_virtual_runtime = current_task_ptr->virtual_runtime;
        if (old_task != next) context_switch_count++;

        // Ensure TSS.RSP0 is updated so Ring 3 -> Ring 0 interrupts use the correct stack!
        // Use get_cpu_index() so APs update their own TSS, not always CPU 0's.
        if (tasks[next]->kstack) {
            set_tss_kstack_for_cpu(cpu_index, kstack_top(tasks[next]->kstack));
        }

        if (tasks[next]->ctx && tasks[next]->ctx != old_ctx) {
            switch_vmm_context(tasks[next]->ctx);
        }

        write_msr(MSR_FS_BASE, tasks[next]->fs_base);
        // The scheduler runs with kernel GS active. Keep the task pointer active
        // and place the next task's user GS in the swapgs shadow register.
        write_msr(MSR_GS_BASE, (uint64_t)current_task_ptr);
        write_msr(MSR_KERNEL_GS_BASE, tasks[next]->gs_base);

        // Eager FPU restore of the incoming task.  clts first so the
        // xrstor/fxrstor doesn't #NM (TS should already be clear in our
        // eager model, but be defensive against any path that set it).
        if (old_task != next && tasks[next]->fpu_area) {
            __asm__ volatile("clts");
            restore_fpu_state(tasks[next]->fpu_area);
        }

        if (exiting && old_ctx && old_ctx != &kernel_context) {
            destroy_vmm_context(old_ctx);
            old->ctx = NULL;
        }

        if (reap_old) {
            old->stack_base = NULL;
            if (old->kstack) {
                deferred_kstacks[cpu_index] = old->kstack;
                old->kstack = NULL;
            }
            if (old->fpu_area) { vfree(old->fpu_area); old->fpu_area = NULL; }
            release_task_slot(old_task);
        }
    }
}

void exit_task(int status) {
    cli();

    process_robust_list(current_task_ptr);
    cleanup_futex_task(current_task);

    wake_clear_child_tid(current_task_ptr);

    // status encodes the exit code; term_sig was already set if killed by signal.
    // For a voluntary exit (sys_exit/sys_exit_group), term_sig stays 0.
    tasks[current_task]->exit_status = status;
    tasks[current_task]->state = TASK_ZOMBIE;
    current_task_ptr->exit_status = status;

    pid_t my_pid  = current_task_ptr->pid;
    pid_t my_ppid = current_task_ptr->ppid;

    // Re-parent children and clean up zombies
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->ppid == my_pid) {
            if (tasks[i]->state == TASK_ZOMBIE) {
                release_task_slot(i);
            } else {
                tasks[i]->ppid = 1; // re-parent to init
            }
        }
    }

    for (int i = 0; i < FD_MAX; i++) {
        if (current_task_ptr->fd_table.entries[i].open) {
            free_fd(&current_task_ptr->fd_table, i);
        }
    }

    if (current_task_ptr->fpu_area) {
        vfree(current_task_ptr->fpu_area);
        current_task_ptr->fpu_area = NULL;
    }

    // Notify parent with SIGCHLD and wake it if it is waiting
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == my_ppid) {
            tasks[i]->pending_signals |= (1ULL << SIGCHLD);
            // Wake parent if it was sleeping in wait4 (it will re-check)
            if (tasks[i]->state == TASK_STOPPED && (tasks[i]->waiting_for == -1 || tasks[i]->waiting_for == my_pid)) {
                tasks[i]->waiting_for = 0;
                tasks[i]->state = TASK_READY;
            } else if (tasks[i]->state == TASK_STOPPED || tasks[i]->state == TASK_READY) {
                tasks[i]->state = TASK_READY;
            }
            break;
        }
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->waiting_for == my_pid) {
            tasks[i]->waiting_for = 0;
            tasks[i]->state = TASK_READY;
            break;
        }
    }

    if (current_task_ptr->pid == 1) panic("init process exited");

    spin_unlock(&sched_lock);
    sti();
    yield_sched();

    idle();
}

bool signal_pending(void) {
    if (!current_task_ptr) return false;
    uint64_t unblockable = (1ULL << 9 /*SIGKILL*/) | (1ULL << 19 /*SIGSTOP*/);
    // blocked_signals is 0-indexed (bit 0 = signal 1), pending_signals is 1-indexed (bit 1 = signal 1)
    uint64_t blocked_shifted = current_task_ptr->blocked_signals << 1;
    return (current_task_ptr->pending_signals & (~blocked_shifted | unblockable)) != 0;
}

void prepare_scheduler_cpu(int cpu_index) {
    if (cpu_index <= 0 || cpu_index >= MAX_CPUS) return;
    pid_t pid = create_task(idle_task, 0, &kernel_context, 0);
    int task_index = task_index_by_pid(pid);
    if (task_index < 0) panic("unable to create cpu idle task");
    task_t *task = tasks[task_index];
    task->pid = 0;
    task->ppid = 0;
    task->state = TASK_RUNNING;
    task->running_cpu = cpu_index;
    task->execution_start_us = get_monotonic_time_us();
    next_pid = 1;
    if (processes_created) processes_created--;
    last_created_pid = 0;
    cpus[cpu_index].task_index = task_index;
    cpus[cpu_index].task = task;
    cpus[cpu_index].idle_task = task_index;
    cpus[cpu_index].minimum_virtual_runtime = 0;
}


void init_sched(void) {
    dead_task = malloc(sizeof(*dead_task));
    if (!dead_task) panic("unable to allocate dead task");
    memset(dead_task, 0, sizeof(*dead_task));
    for (int i = 0; i < MAX_TASKS; i++) tasks[i] = dead_task;
    // Create the idle task at tasks[0] (PID 0)
    create_task(idle_task, 0, &kernel_context, 0);

    current_task = 0;
    current_task_ptr = tasks[0];
    cpus[0].idle_task = 0;
    cpus[0].minimum_virtual_runtime = 0;
    tasks[0]->state = TASK_RUNNING;
    tasks[0]->running_cpu = 0;
    tasks[0]->execution_start_us = get_monotonic_time_us();
    idle_time_us = 0;
    last_account_us = get_monotonic_time_us();
    last_load_update_us = last_account_us;
    memset(load_averages, 0, sizeof(load_averages));
    sched_ready = true;

    log("sched: initialized sched\n");
}
