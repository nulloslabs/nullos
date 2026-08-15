#include <stdint.h>
#include <signal.h>
#include <main/sched.h>
#include <main/signal.h>

bool send_task_signal(int task_index, int signal) {
    if (task_index < 0 || task_index >= MAX_TASKS || signal < 1 || signal > 31) return false;
    task_t *task = tasks[task_index];
    if (task->state == TASK_DEAD || task->state == TASK_ZOMBIE) return false;
    uint64_t handler = task->sigactions[signal * 4];
    bool unblockable = signal == SIGKILL || signal == SIGSTOP;
    bool blocked = !unblockable && (task->blocked_signals & (1ULL << (signal - 1)));
    bool ignored = handler == (uint64_t)SIG_IGN || (handler == (uint64_t)SIG_DFL && (signal == SIGCHLD || signal == SIGCONT || signal == SIGURG || signal == SIGWINCH));

    if (signal == SIGCONT) {
        bool was_stopped = task->state == TASK_STOPPED && task->stopped_by_signal;
        if (task->state == TASK_STOPPED) task->state = TASK_READY;
        task->stopped_by_signal = 0;
        task->stop_reported = 0;
        task->pending_signals &= ~((1ULL << SIGSTOP) | (1ULL << SIGTSTP) | (1ULL << SIGTTIN) | (1ULL << SIGTTOU));
        if (was_stopped) {
            int parent_index = task_index_by_pid(task->ppid);
            if (parent_index >= 0) tasks[parent_index]->pending_signals |= 1ULL << SIGCHLD;
        }
        if (!ignored) task->pending_signals |= 1ULL << signal;
        return true;
    }

    if (!ignored) task->pending_signals |= 1ULL << signal;
    if (task->state == TASK_SLEEPING) { task->sleep_deadline_us = 0; task->state = TASK_READY; }
    if (task->state == TASK_STOPPED && (signal == SIGKILL || (!task->stopped_by_signal && !blocked && !ignored))) task->state = TASK_READY;
    return true;
}
