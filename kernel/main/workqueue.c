#include <stddef.h>
#include <main/panic.h>
#include <main/log.h>
#include <main/sched.h>
#include <main/spinlocks.h>
#include <main/string.h>
#include <main/workqueue.h>

static kernel_work_t kernel_work[KERNEL_WORK_CAPACITY];
static spinlock_t kernel_work_lock = SPINLOCK_INIT;
static uint32_t kernel_work_head;
static uint32_t kernel_work_tail;
static int kernel_worker_index = -1;

bool queue_kernel_work(kernel_work_handler_t handler, void *context) {
    if (!handler) return false;
    uint64_t flags;
    spin_lock_irqsave(&kernel_work_lock, &flags);
    uint32_t next = (kernel_work_head + 1) % KERNEL_WORK_CAPACITY;
    if (next == kernel_work_tail) {
        spin_unlock_irqrestore(&kernel_work_lock, flags);
        return false;
    }
    kernel_work[kernel_work_head].handler = handler;
    kernel_work[kernel_work_head].context = context;
    kernel_work_head = next;
    if (kernel_worker_index >= 0 && tasks[kernel_worker_index]->state == TASK_STOPPED) tasks[kernel_worker_index]->state = TASK_READY;
    spin_unlock_irqrestore(&kernel_work_lock, flags);
    return true;
}

static void process_kernel_workqueue(void) {
    kernel_worker_index = current_task;
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&kernel_work_lock, &flags);
        if (kernel_work_tail == kernel_work_head) {
            current_task_ptr->state = TASK_STOPPED;
            spin_unlock_irqrestore(&kernel_work_lock, flags);
            __asm__ volatile("int $32");
            continue;
        }
        kernel_work_t work = kernel_work[kernel_work_tail];
        kernel_work_tail = (kernel_work_tail + 1) % KERNEL_WORK_CAPACITY;
        spin_unlock_irqrestore(&kernel_work_lock, flags);
        work.handler(work.context);
    }
}

void start_kernel_workqueue(void) {
    pid_t pid = create_task(process_kernel_workqueue, 0, &kernel_context, 0);
    if (pid < 0) panic("unable to create kernel worker (pid %d)", pid);
    task_t *worker = task_by_pid(pid);
    if (worker) strlcpy(worker->name, "kworker", sizeof(worker->name));
    log("workqueue: started kernel workqueue\n");
}
