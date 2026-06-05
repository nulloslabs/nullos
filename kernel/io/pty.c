#include <main/devfs.h>
#include <main/string.h>
#include <main/spinlock.h>
#include <freestanding/errno.h>
#include <io/terminal.h>
#include <io/pty.h>
#include <io/tty.h>

pty_t ptys[NUM_PTYS];
spinlock_t pty_lock = SPINLOCK_INIT;

#define DEFINE_PTS_CALLBACKS(n) \
static uint64_t read_pts##n(void *buf, uint64_t count, uint64_t offset) { \
    (void)offset; \
    pty_t *p = &ptys[n]; \
    char *b = (char *)buf; int got = 0; \
    while (got == 0) { \
        uint64_t irq; spin_lock_irqsave(&pty_lock, &irq); \
        if (!p->allocated || p->master_refs == 0) { \
            spin_unlock_irqrestore(&pty_lock, irq); \
            return 0; \
        } \
        got = read_tty_ring(&p->m2s, b, (int)count); \
        spin_unlock_irqrestore(&pty_lock, irq); \
    } \
    return (uint64_t)got; \
} \
static uint64_t write_pts##n(const void *buf, uint64_t count, uint64_t offset) { \
    (void)offset; \
    pty_t *p = &ptys[n]; \
    uint64_t irq; spin_lock_irqsave(&pty_lock, &irq); \
    if (!p->allocated || p->master_refs == 0) { \
        spin_unlock_irqrestore(&pty_lock, irq); \
        return (uint64_t)-EIO; \
    } \
    int w = write_tty_ring(&p->s2m, (const char *)buf, (int)count); \
    spin_unlock_irqrestore(&pty_lock, irq); \
    return (uint64_t)w; \
}

DEFINE_PTS_CALLBACKS(0)  DEFINE_PTS_CALLBACKS(1)  DEFINE_PTS_CALLBACKS(2)  DEFINE_PTS_CALLBACKS(3)
DEFINE_PTS_CALLBACKS(4)  DEFINE_PTS_CALLBACKS(5)  DEFINE_PTS_CALLBACKS(6)  DEFINE_PTS_CALLBACKS(7)
DEFINE_PTS_CALLBACKS(8)  DEFINE_PTS_CALLBACKS(9)  DEFINE_PTS_CALLBACKS(10) DEFINE_PTS_CALLBACKS(11)
DEFINE_PTS_CALLBACKS(12) DEFINE_PTS_CALLBACKS(13) DEFINE_PTS_CALLBACKS(14) DEFINE_PTS_CALLBACKS(15)

static uint64_t (* const pts_reads[NUM_PTYS])(void*, uint64_t, uint64_t) = {
    read_pts0, read_pts1, read_pts2, read_pts3, read_pts4, read_pts5, read_pts6, read_pts7,
    read_pts8, read_pts9, read_pts10, read_pts11, read_pts12, read_pts13, read_pts14, read_pts15
};

static uint64_t (* const pts_writes[NUM_PTYS])(const void*, uint64_t, uint64_t) = {
    write_pts0, write_pts1, write_pts2, write_pts3, write_pts4, write_pts5, write_pts6, write_pts7,
    write_pts8, write_pts9, write_pts10, write_pts11, write_pts12, write_pts13, write_pts14, write_pts15
};

pty_t *get_pty(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return NULL;
    return &ptys[idx];
}

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
            
            char name[16] = "pts/";
            if (i < 10) { name[4] = '0' + i; name[5] = '\0'; }
            else { name[4] = '1'; name[5] = '0' + (i - 10); name[6] = '\0'; }
            register_devfs_device(name, pts_reads[i], pts_writes[i]);
            
            spin_unlock_irqrestore(&pty_lock, irq);
            return i;
        }
    }
    spin_unlock_irqrestore(&pty_lock, irq);
    return -ENOSPC;
}

static void destroy_pty(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return;
    char name[16] = "pts/";
    if (idx < 10) { name[4] = '0' + idx; name[5] = '\0'; }
    else { name[4] = '1'; name[5] = '0' + (idx - 10); name[6] = '\0'; }
    unregister_devfs_device(name);

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
    if (ptys[idx].allocated && ptys[idx].master_refs > 0) {
        ptys[idx].master_refs--;
        destroy = (ptys[idx].master_refs == 0 && ptys[idx].slave_refs == 0);
    }
    spin_unlock_irqrestore(&pty_lock, irq);

    if (destroy)
        destroy_pty(idx);
}

int open_pty_slave(int idx) {
    if (idx < 0 || idx >= NUM_PTYS) return -ENOENT;

    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (!ptys[idx].allocated || ptys[idx].master_refs == 0) {
        spin_unlock_irqrestore(&pty_lock, irq);
        return -EIO;
    }
    if (ptys[idx].locked) {
        spin_unlock_irqrestore(&pty_lock, irq);
        return -EIO;
    }
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
    if (ptys[idx].slave_refs > 0) {
        ptys[idx].slave_refs--;
        destroy = (ptys[idx].master_refs == 0 && ptys[idx].slave_refs == 0);
    }
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
    while (*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0' || idx >= NUM_PTYS) return -1;
    return idx;
}

int read_pty_master(int idx, char *buf, int len) {
    pty_t *p = get_pty(idx);
    if (!p) return -1;
    int got = 0;
    while (got == 0) {
        uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
        if (!p->allocated || p->slave_refs == 0) {
            spin_unlock_irqrestore(&pty_lock, irq);
            return -EIO;
        }
        got = read_tty_ring(&p->s2m, buf, len);
        spin_unlock_irqrestore(&pty_lock, irq);
    }
    return got;
}

int write_pty_master(int idx, const char *buf, int len) {
    pty_t *p = get_pty(idx);
    if (!p) return -1;
    uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
    if (!p->allocated) {
        spin_unlock_irqrestore(&pty_lock, irq);
        return -1;
    }
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
