#include <errno.h>
#include <signal.h>
#include <main/sched.h>
#include <main/signal.h>
#include <io/pts_devices.h>
#include <io/pty.h>
#include <io/devpts.h>

static bool pts_current_pgrp_orphaned(void) {
    pid_t my_pgid = current_task_ptr->pgid;
    pid_t my_sid = current_task_ptr->sid;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *m = tasks[i];
        if (!m || m->state == TASK_DEAD) continue;
        if (m->pgid != my_pgid) continue;
        if (m->sid != my_sid) continue;
        task_t *parent = task_by_pid(m->ppid);
        if (!parent || parent->state == TASK_DEAD) continue;
        if (parent->sid == my_sid && parent->pgid != my_pgid) return false;
    }
    return true;
}

static bool pts_sigttin_blocked_or_ignored(void) {
    uint64_t handler = current_task_ptr->sigactions[SIGTTIN * 4];
    if (handler == (uint64_t)SIG_IGN) return true;
    if (current_task_ptr->blocked_signals & (1ULL << (SIGTTIN - 1))) return true;
    return false;
}

static void pts_signal_current_pgrp(int sig) {
    pid_t my_pgid = current_task_ptr->pgid;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *m = tasks[i];
        if (!m || m->state == TASK_DEAD) continue;
        if (m->pgid != my_pgid) continue;
        send_task_signal(i, sig);
    }
}

static uint64_t read_pts(int idx, void *buf, uint64_t count, uint64_t offset) {
    (void)offset;
    if (idx < 0 || idx >= NUM_PTYS) return (uint64_t)-EINVAL;
    pty_t *p = &ptys[idx];
    // Job control: a background read from the controlling terminal must stop
    // the READER's process group with SIGTTIN (not the foreground group),
    // matching the TTY path. Orphaned groups or readers blocking/ignoring
    // SIGTTIN get EIO instead, per POSIX.
    if (p->fg_pgrp > 0 && current_task_ptr->pgid != p->fg_pgrp &&
        current_task_ptr->ctty_idx == 100 + idx) {
        if (pts_current_pgrp_orphaned() || pts_sigttin_blocked_or_ignored())
            return (uint64_t)-EIO;
        pts_signal_current_pgrp(SIGTTIN);
        return (uint64_t)-EINTR;
    }
    char *b = (char *)buf; int got = 0;
    while (got == 0) {
        uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
        if (!p->allocated || p->master_refs == 0) {
            spin_unlock_irqrestore(&pty_lock, irq);
            return 0;
        }
        got = read_tty_ring(&p->m2s, b, (int)count);
        spin_unlock_irqrestore(&pty_lock, irq);

        if (got == 0) {
            if (signal_pending()) return (uint64_t)-EINTR;
            let_current_task_sleep(1000);
        }
    }
    return (uint64_t)got;
}

static uint64_t write_pts(int idx, const void *buf, uint64_t count, uint64_t offset) {
    (void)offset;
    if (idx < 0 || idx >= NUM_PTYS) return (uint64_t)-EINVAL;
    pty_t *p = &ptys[idx];
    uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
    if (!p->allocated || p->master_refs == 0) {
        spin_unlock_irqrestore(&pty_lock, irq);
        return (uint64_t)-EIO;
    }
    int w = write_tty_ring(&p->s2m, (const char *)buf, (int)count);
    spin_unlock_irqrestore(&pty_lock, irq);
    return (uint64_t)w;
}

uint64_t read_pts_device(const char* name, void* buf, uint64_t count, uint64_t offset) {
    int idx = get_pts_idx(name);
    if (idx < 0) return (uint64_t)-ENOENT;
    return read_pts(idx, buf, count, offset);
}

uint64_t write_pts_device(const char* name, const void* buf, uint64_t count, uint64_t offset) {
    int idx = get_pts_idx(name);
    if (idx < 0) return (uint64_t)-ENOENT;
    return write_pts(idx, buf, count, offset);
}
