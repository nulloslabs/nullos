#include <freestanding/stddef.h>
#include <main/string.h>
#include <main/scheduler.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <main/errno.h>
#include <main/halt.h>
#include <main/panic.h>
#include <main/gdt.h>
#include <main/spinlock.h>
#include <io/terminal.h>
#include <main/fd.h>
#include <io/usb.h>

task_t tasks[MAX_TASKS];
int current_task = 0;
task_t* current_task_ptr = &tasks[0];
volatile int sched_lock = 0;
static spinlock_t task_lock = SPINLOCK_INIT;

// WARNING: NOT TO BE USED BY KERNEL, ONLY USED AS A "SCRATCH TOY" FOR CPU!!
static void idle_task(void) {
    idle();
}

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
                stack = vmalloc(32768);
                tasks[i].ctx = ctx ? ctx : &kernel_context;
                tasks[i].kernel_stack = stack;
            } else {
                cs = 0x23;
                ss = 0x1B;
                stack = vmalloc_user_ex(ctx, 16384);
                if (!stack) {
                    spin_unlock_irqrestore(&task_lock, flags);
                    return -1;
                }
                tasks[i].ctx = ctx;
                tasks[i].kernel_stack = vmalloc(32768);
            }

            tasks[i].stack_base = stack;
            tasks[i].ring = ring;
            tasks[i].ctx = ctx ? ctx : &kernel_context;

            init_fd_table(&tasks[i].fd_table);
            strcpy(tasks[i].cwd, "/");

            uint64_t v_rsp;
            if (ring == 0) {
                v_rsp = initial_rsp ? initial_rsp : ((uint64_t)stack + 32768);
            } else {
                v_rsp = initial_rsp;
            }

            uint64_t k_rsp = (uint64_t)tasks[i].kernel_stack + 32768;

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
            tasks[i].parent_pid = current_task_ptr->pid;
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
            tasks[i].parent_pid = current_task_ptr->pid;

            void *kstack = vmalloc(32768);
            if (!kstack) {
                spin_unlock_irqrestore(&task_lock, flags);
                return -1;
            }
            tasks[i].kernel_stack = kstack;

            memcpy(&tasks[i].fd_table, &current_task_ptr->fd_table, sizeof(fd_table_t));

            uint64_t v_rsp = (uint64_t)kstack + 32768;

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

    if (tasks[old_task].state == TASK_ZOMBIE && tasks[old_task].parent_pid == 0) {
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
            tss_set_kernel_stack_for_cpu(0, (void*)((uint64_t)tasks[next].kernel_stack + 32768));
        }

        if (tasks[next].ctx && tasks[next].ctx != tasks[old_task].ctx) {
            switch_vmm_context(tasks[next].ctx);
        }
    }
}

void exit_task(int status) {
    cli();

    tasks[current_task].exit_status = status;
    tasks[current_task].state = TASK_ZOMBIE;
    current_task_ptr->exit_status = status;

    pid_t my_pid = current_task_ptr->pid;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].parent_pid == my_pid) {
            if (tasks[i].state == TASK_ZOMBIE) {
                tasks[i].state = TASK_DEAD;
            } else {
                tasks[i].parent_pid = 1; // re-parent to init
            }
        }
    }

    for (int i = 0; i < FD_MAX; i++) {
        if (current_task_ptr->fd_table.entries[i].open) {
            free_fd(&current_task_ptr->fd_table, i);
        }
    }

    if (current_task_ptr->pid == 1) {
        panic("Init process exited.");
    }

    asm volatile("int $32");

    idle();
}

void init_scheduler(void) {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].state = TASK_DEAD;
    // Create the idle task at tasks[0] (PID 0)
    create_task(idle_task, 0, &kernel_context, 0);

    current_task = 0;
    current_task_ptr = &tasks[0];

    printf("Scheduler: Initialized scheduler.\n");
}