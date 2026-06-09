#include <freestanding/stddef.h>
#include <main/string.h>
#include <main/scheduler.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <freestanding/errno.h>
#include <main/halt.h>
#include <main/panic.h>
#include <main/gdt.h>
#include <main/spinlock.h>
#include <io/terminal.h>
#include <main/fd.h>
#include <main/msr.h>
#include <io/usb.h>

task_t tasks[MAX_TASKS];
int current_task = 0;
task_t* current_task_ptr = &tasks[0];
static spinlock_t task_lock = SPINLOCK_INIT;

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

            init_fd_table(&tasks[i].fd_table);
            strcpy(tasks[i].cwd, "/");

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
            tasks[i].pid = (pid_t)i;
            tasks[i].ppid = current_task_ptr->pid;
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
            tasks[i].uid = current_task_ptr->uid;
            tasks[i].euid = current_task_ptr->euid;
            tasks[i].gid = current_task_ptr->gid;
            tasks[i].egid = current_task_ptr->egid;
            tasks[i].fs_base = current_task_ptr->fs_base;
            tasks[i].gs_base = current_task_ptr->gs_base;

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
            PUSH(frame->r12);
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
        tasks[old_task].state = TASK_DEAD;
        tasks[old_task].pid = 0;
    }

    if (found) {
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
    }
}

void exit_task(int status) {
    cli();

    tasks[current_task].exit_status = status;
    tasks[current_task].state = TASK_ZOMBIE;
    current_task_ptr->exit_status = status;

    pid_t my_pid = current_task_ptr->pid;
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
