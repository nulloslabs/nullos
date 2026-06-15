#include <main/devpts.h>
#include <main/string.h>
#include <io/pty.h>
#include <freestanding/errno.h>

static int get_pts_idx(const char *name) {
    if (!name || *name == '\0') return -1;
    if (strcmp(name, "ptmx") == 0) return -1; // handled specially or in devfs

    int idx = 0;
    const char *p = name;
    while (*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0') return -1;
    if (idx < 0 || idx >= NUM_PTYS) return -1;
    return idx;
}

uint64_t read_devpts(const char* name, void* buf, uint64_t count, uint64_t offset) {
    int idx = get_pts_idx(name);
    if (idx < 0) return (uint64_t)-ENOENT;
    return read_pts(idx, buf, count, offset);
}

uint64_t write_devpts(const char* name, const void* buf, uint64_t count, uint64_t offset) {
    int idx = get_pts_idx(name);
    if (idx < 0) return (uint64_t)-ENOENT;
    return write_pts(idx, buf, count, offset);
}

bool devpts_device_exists(const char* name) {
    int idx = get_pts_idx(name);
    if (idx < 0) return false;
    
    bool exists = false;
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (ptys[idx].allocated) exists = true;
    spin_unlock_irqrestore(&pty_lock, irq);
    return exists;
}

const char *devpts_get_device_name(int index) {
    if (index < 0 || index >= NUM_PTYS) return NULL;
    
    static char names[NUM_PTYS][4];
    
    uint64_t irq;
    spin_lock_irqsave(&pty_lock, &irq);
    if (!ptys[index].allocated) {
        spin_unlock_irqrestore(&pty_lock, irq);
        return NULL;
    }
    spin_unlock_irqrestore(&pty_lock, irq);

    if (index < 10) {
        names[index][0] = '0' + index;
        names[index][1] = '\0';
    } else {
        names[index][0] = '1';
        names[index][1] = '0' + (index - 10);
        names[index][2] = '\0';
    }
    return names[index];
}
