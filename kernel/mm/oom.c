#include <stddef.h>
#include <signal.h>
#include <main/log.h>
#include <main/panic.h>
#include <main/sched.h>
#include <main/signal.h>
#include <mm/mm.h>
#include <mm/vma.h>
#include <mm/oom.h>

void kill_oom(void) {
    uint64_t max_usage = 0;
    int target_idx = -1;

    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->ring == 0) continue;
        if (tasks[i]->pid == 0 || tasks[i]->pid == 1) continue;
        if (tasks[i]->state == TASK_DEAD || tasks[i]->state == TASK_ZOMBIE) continue;

        uint64_t usage = 0;
        for (int j = 0; j < VMA_MAX; j++) {
            if (tasks[i]->vmas.entries[j].used) {
                usage += (tasks[i]->vmas.entries[j].end - tasks[i]->vmas.entries[j].start);
            }
        }

        if (usage > max_usage) {
            max_usage = usage;
            target_idx = i;
        }
    }

    if (target_idx != -1) {
        log("oom: out of memory, killing process '%s' (pid %d)\n", tasks[target_idx]->name, tasks[target_idx]->pid);
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->ring == 0) continue;
            if (tasks[i]->state == TASK_DEAD || tasks[i]->state == TASK_ZOMBIE) continue;
            if (tasks[i]->pgid == tasks[target_idx]->pgid) send_task_signal(i, SIGKILL);
        }
    } else {
        panic("out of memory");
    }

    spin_unlock_irqrestore(&sched_lock, flags);
}
