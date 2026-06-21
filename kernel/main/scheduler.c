#include <freestanding/stddef.h>
#include <freestanding/errno.h>
#include <main/string.h>
#include <main/halt.h>
#include <main/panic.h>
#include <main/gdt.h>
#include <main/spinlocks.h>
#include <main/scheduler.h>
#include <main/sse.h>
#include <main/fd.h>
#include <main/msr.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <io/terminal.h>
#include <io/usb.h>
#include <syscalls/syscall_impls.h>

task_t tasks[MAX_TASKS];
int current_task = 0;
task_t* current_task_ptr = &tasks[0];
static spinlock_t task_lock = SPINLOCK_INIT;
static pid_t next_pid = 0;

// Let's keep this public and not private, other functions change it.
spinlock_t sched_lock = SPINLOCK_INIT;

static void idle_task(void) { idle(); }

pid_t create_task(void (*entry)(void), uint8_t ring, vmm_context_t *ctx, uint64_t initial_rsp) {
    uint64_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) {
            uint64_t cs, ss;
            void *stack;

            if (ring == 0) {
                cs = 0x08;
                ss = 0x10;
                stack = vmalloc(KERNEL_STACK_SIZE);
                tasks[i].ctx = ctx ? ctx : &kernel_context;
                tasks[i].kernel_stack = stack;
            } else {
                cs = 0x23;
                ss = 0x1B;
                if (initial_rsp) {
                    stack = NULL;
                } else {
                    stack = vmalloc_user_ex(ctx, USER_STACK_SIZE);
                    if (!stack) {
                        spin_unlock_irqrestore(&task_lock, flags);
                        return -1;
                    }
                }
                tasks[i].ctx = ctx;
                tasks[i].kernel_stack = vmalloc(KERNEL_STACK_SIZE);
            }

            tasks[i].stack_base = stack;
            tasks[i].ring = ring;
            tasks[i].ctx = ctx ? ctx : &kernel_context;
            tasks[i].uid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->uid;
            tasks[i].euid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->euid;
            tasks[i].gid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->gid;
            tasks[i].egid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->egid;
            tasks[i].fs_base = 0;
            tasks[i].gs_base = 0;
            tasks[i].ctty_idx = current_task_ptr ? current_task_ptr->ctty_idx : 0;

            init_fd_table(&tasks[i].fd_table);
            strcpy(tasks[i].cwd, "/");
            memset(tasks[i].sigactions, 0, sizeof(tasks[i].sigactions));
            tasks[i].pgid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->pgid;
            tasks[i].sid = (i == 0 || !current_task_ptr) ? 0 : current_task_ptr->sid;
            tasks[i].term_sig = 0;
            tasks[i].pending_signals = 0;
            tasks[i].blocked_signals = 0;
            tasks[i].on_altstack = false;
            tasks[i].sas_ss_sp = NULL;
            tasks[i].sas_ss_size = 0;
            tasks[i].sas_ss_flags = 0;

            // Per-task FPU area: freshly initialized to a clean state so the
            // new task starts with sane x87/SSE registers rather than the
            // previous owner's.  Kernel tasks (ring 0) also get one so any
            // FP use inside the kernel doesn't leak across tasks.
            tasks[i].fpu_area = vmalloc(get_fpu_state_size());
            if (tasks[i].fpu_area) {
                init_fpu_area(tasks[i].fpu_area);
            }

            uint64_t v_rsp;
            if (ring == 0) {
                v_rsp = initial_rsp ? initial_rsp : ((uint64_t)stack + KERNEL_STACK_SIZE);
            } else {
                v_rsp = initial_rsp ? initial_rsp : ((uint64_t)stack + USER_STACK_SIZE);
            }

            uint64_t k_rsp = (uint64_t)tasks[i].kernel_stack + KERNEL_STACK_SIZE;

            #define PUSH(val) do { \
                k_rsp -= 8; \
                uint64_t _pv = (uint64_t)(val); \
                write_vmm(&kernel_context, k_rsp, &_pv, 8); \
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

            tasks[i].rsp = k_rsp;
            tasks[i].pid = next_pid++;
            tasks[i].ppid = current_task_ptr ? current_task_ptr->pid : 0;
            tasks[i].state = TASK_READY;
            tasks[i].priority = 1;

            spin_unlock_irqrestore(&task_lock, flags);
            return tasks[i].pid;
        }
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return -EAGAIN;
}

pid_t clone_task(syscall_frame_t *frame, vmm_context_t *child_ctx) {
    uint64_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) {
            tasks[i].stack_base = current_task_ptr->stack_base;
            tasks[i].ring = current_task_ptr->ring;
            tasks[i].ctx = child_ctx;
            strcpy(tasks[i].cwd, current_task_ptr->cwd);
            tasks[i].ppid = current_task_ptr->pid;
            tasks[i].pgid = current_task_ptr->pgid;
            tasks[i].uid = current_task_ptr->uid;
            tasks[i].euid = current_task_ptr->euid;
            tasks[i].gid = current_task_ptr->gid;
            tasks[i].egid = current_task_ptr->egid;
            tasks[i].fs_base = current_task_ptr->fs_base;
            tasks[i].gs_base = current_task_ptr->gs_base;
            tasks[i].ctty_idx = current_task_ptr->ctty_idx;

            // Inherit signal dispositions from parent (POSIX fork semantics)
            memcpy(tasks[i].sigactions, current_task_ptr->sigactions, sizeof(tasks[i].sigactions));
            tasks[i].blocked_signals = current_task_ptr->blocked_signals;
            tasks[i].pending_signals = 0;
            tasks[i].sid = current_task_ptr->sid;
            tasks[i].term_sig = 0;
            tasks[i].on_altstack = false;
            tasks[i].sas_ss_sp = current_task_ptr->sas_ss_sp;
            tasks[i].sas_ss_size = current_task_ptr->sas_ss_size;
            tasks[i].sas_ss_flags = current_task_ptr->sas_ss_flags;

            // Per-task FPU area.  The child inherits the parent's current FP
            // state (POSIX fork: child sees a snapshot of the parent's FPU).
            // We save the live state into the child's fresh buffer.
            tasks[i].fpu_area = vmalloc(get_fpu_state_size());
            if (tasks[i].fpu_area) {
                init_fpu_area(tasks[i].fpu_area);
                // Copy the parent's last-saved FPU state into the child.
                if (current_task_ptr->fpu_area) {
                    memcpy(tasks[i].fpu_area, current_task_ptr->fpu_area, get_fpu_state_size());
                }
            }

            void *kstack = vmalloc(KERNEL_STACK_SIZE);
            if (!kstack) {
                spin_unlock_irqrestore(&task_lock, flags);
                return -1;
            }
            tasks[i].kernel_stack = kstack;

            memcpy(&tasks[i].fd_table, &current_task_ptr->fd_table, sizeof(fd_table_t));
            for (int fd = 0; fd < FD_MAX; fd++) {
                if (tasks[i].fd_table.entries[fd].open)
                    retain_fd_entry(&tasks[i].fd_table.entries[fd]);
            }

            uint64_t v_rsp = (uint64_t)kstack + KERNEL_STACK_SIZE;

            #define PUSH(val) do { \
                v_rsp -= 8; \
                uint64_t _pv = (uint64_t)(val); \
                write_vmm(&kernel_context, v_rsp, &_pv, 8); \
            } while(0)

            PUSH(0x1B);
            PUSH(frame->rsp);
            PUSH(frame->r11);
            PUSH(0x23);
            PUSH(frame->rcx);

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

            tasks[i].rsp = v_rsp;
            tasks[i].pid = (pid_t)i;
            tasks[i].state = TASK_READY;
            tasks[i].priority = 1;

            spin_unlock_irqrestore(&task_lock, flags);
            return tasks[i].pid;
        }
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return -EAGAIN;
}

void schedule(void) {
    futex_check_timeouts();

    int old_task = current_task;

    if (tasks[old_task].state == TASK_RUNNING) {
        tasks[old_task].state = TASK_READY;
    }

    int next = old_task;
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].state == TASK_READY) {
            found = 1;
            break;
        }
    }

    if (tasks[old_task].state == TASK_ZOMBIE && tasks[old_task].ppid == 0) {
        if (tasks[old_task].stack_base) {
            free(tasks[old_task].stack_base);
            tasks[old_task].stack_base = NULL;
        }
        if (tasks[old_task].kernel_stack && tasks[old_task].ring != 0) {
            free(tasks[old_task].kernel_stack);
            tasks[old_task].kernel_stack = NULL;
        }
        if (tasks[old_task].fpu_area) {
            vfree(tasks[old_task].fpu_area);
            tasks[old_task].fpu_area = NULL;
        }
        tasks[old_task].state = TASK_DEAD;
        tasks[old_task].pid = 0;
    }

    if (found) {
        // Eager FPU save of the outgoing task (before its registers are
        // clobbered by the incoming task).  Skip if the outgoing task is a
        // zombie being reaped above — its fpu_area is already gone.
        if (old_task != next && tasks[old_task].fpu_area &&
            tasks[old_task].state != TASK_DEAD) {
            save_fpu_state(tasks[old_task].fpu_area);
        }

        current_task = next;
        tasks[current_task].state = TASK_RUNNING;
        current_task_ptr = &tasks[current_task];

        // Ensure TSS.RSP0 is updated so Ring 3 -> Ring 0 interrupts use the correct stack!
        if (tasks[next].kernel_stack) {
            tss_set_kernel_stack_for_cpu(0, (void*)((uint64_t)tasks[next].kernel_stack + KERNEL_STACK_SIZE));
        }

        if (tasks[next].ctx && tasks[next].ctx != tasks[old_task].ctx) {
            switch_vmm_context(tasks[next].ctx);
        }

        write_msr(MSR_FS_BASE, tasks[next].fs_base);
        write_msr(MSR_KERNEL_GS_BASE, tasks[next].gs_base);
        write_msr(MSR_GS_BASE, (uint64_t)current_task_ptr);

        // Eager FPU restore of the incoming task.  clts first so the
        // xrstor/fxrstor doesn't #NM (TS should already be clear in our
        // eager model, but be defensive against any path that set it).
        if (old_task != next && tasks[next].fpu_area) {
            __asm__ volatile("clts");
            restore_fpu_state(tasks[next].fpu_area);
        }
    }
}

void exit_task(int status) {
    cli();

    wake_clear_child_tid(current_task_ptr);

    // status encodes the exit code; term_sig was already set if killed by signal.
    // For a voluntary exit (sys_exit/sys_exit_group), term_sig stays 0.
    tasks[current_task].exit_status = status;
    tasks[current_task].state = TASK_ZOMBIE;
    current_task_ptr->exit_status = status;

    pid_t my_pid  = current_task_ptr->pid;
    pid_t my_ppid = current_task_ptr->ppid;

    // Re-parent children and clean up zombies
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].ppid == my_pid) {
            if (tasks[i].state == TASK_ZOMBIE) {
                tasks[i].state = TASK_DEAD;
            } else {
                tasks[i].ppid = 1; // re-parent to init
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
        if (tasks[i].state != TASK_DEAD && tasks[i].pid == my_ppid) {
            tasks[i].pending_signals |= (1ULL << SIGCHLD);
            // Wake parent if it was sleeping in wait4 (it will re-check)
            if (tasks[i].state == TASK_STOPPED || tasks[i].state == TASK_READY)
                tasks[i].state = TASK_READY;
            break;
        }
    }

    if (current_task_ptr->pid == 1) {
        panic("init process exited");
    }

    spin_unlock(&sched_lock);
    sti();
    __asm__ volatile("int $32");

    idle();
}

void init_scheduler(void) {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].state = TASK_DEAD;
    // Create the idle task at tasks[0] (PID 0)
    create_task(idle_task, 0, &kernel_context, 0);

    current_task = 0;
    current_task_ptr = &tasks[0];

    printf("scheduler: initialized scheduler\n");
}
