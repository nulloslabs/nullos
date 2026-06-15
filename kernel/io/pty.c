#include <main/devfs.h>
#include <main/string.h>
#include <main/spinlock.h>
#include <freestanding/errno.h>
#include <io/terminal.h>
#include <io/pty.h>
#include <io/tty.h>

pty_t ptys[NUM_PTYS];
spinlock_t pty_lock = SPINLOCK_INIT;

uint64_t read_pts(int idx, void *buf, uint64_t count, uint64_t offset) {
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
    }
    return (uint64_t)got;
}

uint64_t write_pts(int idx, const void *buf, uint64_t count, uint64_t offset) {
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

pty_t *get_pty(int idx) { if (idx < 0 || idx >= NUM_PTYS) return NULL; return &ptys[idx]; }

int alloc_pty(void) {
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    for (int i = 0; i < NUM_PTYS; i++) {
        if (!ptys[i].allocated) {
            ptys[i].allocated = true;
            ptys[i].locked = true;
            ptys[i].master_refs = 1;
            ptys[i].slave_refs = 0;
            ptys[i].m2s.head = ptys[i].m2s.tail = 0;
            ptys[i].s2m.head = ptys[i].s2m.tail = 0;
            
            spin_unlock_irqrestore(&pty_lock, irq);
            return i;
        }
    }
    spin_unlock_irqrestore(&pty_lock, irq);
    return -ENOSPC;
}

static void destroy_pty(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return;

    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    ptys[idx].allocated = false;
    ptys[idx].locked = false;
    ptys[idx].master_refs = 0;
    ptys[idx].slave_refs = 0;
    spin_unlock_irqrestore(&pty_lock, irq);
}

void retain_pty_master(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return;
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (ptys[idx].allocated)
        ptys[idx].master_refs++;
    spin_unlock_irqrestore(&pty_lock, irq);
}

void release_pty_master(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return;

    bool destroy = false;
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (ptys[idx].allocated && ptys[idx].master_refs > 0) { ptys[idx].master_refs--; destroy = (ptys[idx].master_refs == 0 && ptys[idx].slave_refs == 0); }
    spin_unlock_irqrestore(&pty_lock, irq);

    if (destroy)
        destroy_pty(idx);
}

int open_pty_slave(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return -ENOENT;

    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (!ptys[idx].allocated || ptys[idx].master_refs == 0) { spin_unlock_irqrestore(&pty_lock, irq); return -EIO; }
    if (ptys[idx].locked) { spin_unlock_irqrestore(&pty_lock, irq); return -EIO; }
    ptys[idx].slave_refs++;
    spin_unlock_irqrestore(&pty_lock, irq);
    return 0;
}

void retain_pty_slave(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return;
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (ptys[idx].allocated)
        ptys[idx].slave_refs++;
    spin_unlock_irqrestore(&pty_lock, irq);
}

void release_pty_slave(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return;
    bool destroy = false;
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (ptys[idx].slave_refs > 0) { ptys[idx].slave_refs--; destroy = (ptys[idx].master_refs == 0 && ptys[idx].slave_refs == 0); }
    spin_unlock_irqrestore(&pty_lock, irq);

    if (destroy)
        destroy_pty(idx);
}

int pty_slave_path_idx(const char *path) {
    if (!path) return -1;
    const char *p = path;
    while (*p == '.' || *p == '/') p++;
    if (strncmp(p, "dev/", 4) == 0) p += 4;
    if (strncmp(p, "pts/", 4) != 0) return -1;
    p += 4;
    if (*p < '0' || *p > '9') return -1;

    int idx = 0;
    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
    if (*p != '\0' || idx >= NUM_PTYS) return -1;
    return idx;
}

int read_pty_master(int idx, char *buf, int len) {
    pty_t *p = get_pty(idx);
    if (!p) return -1;
    int got = 0;
    while (got == 0) {
        uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
        if (!p->allocated || p->slave_refs == 0) { spin_unlock_irqrestore(&pty_lock, irq); return -EIO; }
        got = read_tty_ring(&p->s2m, buf, len);
        spin_unlock_irqrestore(&pty_lock, irq);
    }
    return got;
}

int write_pty_master(int idx, const char *buf, int len) {
    pty_t *p = get_pty(idx);
    if (!p) return -1;
    uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
    if (!p->allocated) { spin_unlock_irqrestore(&pty_lock, irq); return -1; }
    int w = write_tty_ring(&p->m2s, buf, len);
    spin_unlock_irqrestore(&pty_lock, irq);
    return w;
}

void init_ptys(void) {
    for (int i = 0; i < NUM_PTYS; i++) {
        ptys[i].allocated = false;
        ptys[i].locked = false;
        ptys[i].master_refs = 0;
        ptys[i].slave_refs = 0;
    }
}