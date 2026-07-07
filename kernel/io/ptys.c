#include <io/devtmpfs.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <freestanding/errno.h>
#include <io/terminal.h>
#include <io/ptys.h>
#include <io/ttys.h>

#include <freestanding/signal.h>
#include <main/sched.h>

pty_t ptys[NUM_PTYS];
spinlock_t pty_lock = SPINLOCK_INIT;

int pty_signal_pgrp(int pty_idx, int sig) {
    if (sig < 1 || sig > 31) return 0;
    pty_t *p = get_pty(pty_idx);
    int delivered = 0;
    if (!p || p->fg_pgrp == 0) {
        if (current_task_ptr) {
            uint64_t handler = current_task_ptr->sigactions[sig * 4];
            if (handler != 1) {
                current_task_ptr->pending_signals |= (1ULL << sig);
                delivered = 1;
            }
        }
        return delivered;
    }
    pid_t fpgrp = p->fg_pgrp;
    int target_ctty = 100 + pty_idx;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) continue;
        if (tasks[i].ctty_idx != target_ctty) continue;
        if (tasks[i].pgid != fpgrp) continue;
        uint64_t handler = tasks[i].sigactions[sig * 4];
        if (handler == 1) continue;
        delivered++;
        if (sig == SIGTSTP || sig == SIGSTOP) {
            if (handler == 0) {
                if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_READY) {
                    tasks[i].state = TASK_STOPPED;
                    tasks[i].stopped_by_signal = 1;
                    tasks[i].stop_reported = 0;
                    for (int j = 0; j < MAX_TASKS; j++) {
                        if (tasks[j].state != TASK_DEAD && tasks[j].pid == tasks[i].ppid) {
                            tasks[j].pending_signals |= (1ULL << SIGCHLD);
                            break;
                        }
                    }
                }
            } else {
                tasks[i].pending_signals |= (1ULL << sig);
            }
        } else {
            tasks[i].pending_signals |= (1ULL << sig);
            if (tasks[i].state == TASK_STOPPED) tasks[i].state = TASK_READY;
        }
    }
    return delivered;
}

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

        if (got == 0) {
            if (signal_pending()) return (uint64_t)-EINTR;
            __asm__ volatile("int $32");
        }
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
            
            ptys[i].termios.c_iflag = 0x2400; // ICRNL | IXON
            ptys[i].termios.c_oflag = 0x05;   // OPOST | ONLCR
            ptys[i].termios.c_cflag = 0x00BF; // CREAD | CS8
            ptys[i].termios.c_lflag = 0x8A3B; // ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN
            ptys[i].termios.c_cc[VINTR] = 3;  // Ctrl+C
            ptys[i].termios.c_cc[VEOF] = 4;   // Ctrl+D
            ptys[i].termios.c_cc[VKILL] = 21; // Ctrl+U
            ptys[i].termios.c_cc[VERASE] = 127; // DEL
            ptys[i].termios.c_cc[VSUSP] = 26; // Ctrl+Z
            ptys[i].fg_pgrp = 0;

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

    clear_keyboard_pty(idx);

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
        if (!p->allocated) { spin_unlock_irqrestore(&pty_lock, irq); return -1; }
        got = read_tty_ring(&p->s2m, buf, len);
        spin_unlock_irqrestore(&pty_lock, irq);

        if (got == 0) {
            if (signal_pending()) return -EINTR;
            __asm__ volatile("int $32");
        }
    }
    return got;
}

int write_pty_master(int idx, const char *buf, int len) {
    pty_t *p = get_pty(idx);
    if (!p) return -1;
    uint64_t irq; spin_lock_irqsave(&pty_lock, &irq);
    if (!p->allocated) { spin_unlock_irqrestore(&pty_lock, irq); return -1; }
    
    tcflag_t lflags = p->termios.c_lflag;
    cc_t vintr = p->termios.c_cc[VINTR];
    cc_t vsusp = p->termios.c_cc[VSUSP];
    
    int w = 0;
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        int sig = 0;
        if ((lflags & ISIG) && vintr && c == (char)vintr) sig = SIGINT;
        else if ((lflags & ISIG) && vsusp && c == (char)vsusp) sig = SIGTSTP;
        
        if (sig) {
            pty_signal_pgrp(idx, sig);
            // Flush the master-to-slave ring buffer on signal (unless NOFLSH).
            // Matches Linux __isig() behaviour.
            if (!(lflags & NOFLSH)) {
                p->m2s.head = p->m2s.tail = 0;
            }
            if (lflags & ECHO) {
                // Echo ^C/^Z to s2m (slave to master), followed by newline
                // so the shell prompt appears on a fresh line (matches Linux
                // __isig() behaviour and our TTY path).
                char caret = '^';
                char letter = (sig == SIGINT) ? 'C' : 'Z';
                char nl = '\n';
                write_tty_ring(&p->s2m, &caret, 1);
                write_tty_ring(&p->s2m, &letter, 1);
                write_tty_ring(&p->s2m, &nl, 1);
            }
            continue; // POSIX: ISIG discards the character
        }
        write_tty_ring(&p->m2s, &c, 1);
        w++;
    }
    
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