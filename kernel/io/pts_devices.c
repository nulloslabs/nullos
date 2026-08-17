#include <errno.h>
#include <main/sched.h>
#include <io/pts_devices.h>
#include <io/pty.h>
#include <io/devpts.h>

static uint64_t read_pts(int idx, void *buf, uint64_t count, uint64_t offset) {
    (void)offset;
    if (idx < 0 || idx >= NUM_PTYS) return (uint64_t)-EINVAL;
    pty_t *p = &ptys[idx];
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
