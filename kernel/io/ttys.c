#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <freestanding/signal.h>
#include <main/spinlocks.h>
#include <main/scheduler.h>
#include <io/ttys.h>
#include <io/terminal.h>

// scancode_to_ascii() lives in keyboard.c; called from ISR context
extern char scancode_to_ascii(uint8_t sc);

static tty_t ttys[NUM_TTYS];
spinlock_t tty_lock = SPINLOCK_INIT;

// Track which TTY receives keyboard input (no VT switching yet, so we use the last active TTY)
static int keyboard_tty = 0;

int get_tty_ring_count(tty_ring_t *r) { return (int)((r->head - r->tail + TTY_BUF_SIZE) % TTY_BUF_SIZE); }

int write_tty_ring(tty_ring_t *r, const char *buf, int len) {
    int written = 0;
    while (written < len) {
        uint32_t next = (r->head + 1) % TTY_BUF_SIZE;
        if (next == r->tail) break;
        r->buf[r->head] = buf[written++];
        r->head = next;
    }
    return written;
}

int read_tty_ring(tty_ring_t *r, char *buf, int len) {
    int read = 0;
    while (read < len && r->tail != r->head) { buf[read++] = r->buf[r->tail]; r->tail = (r->tail + 1) % TTY_BUF_SIZE; }
    return read;
}

tty_t *get_tty(int idx) { if (idx < 0 || idx >= NUM_TTYS) return NULL; return &ttys[idx]; }

void tty_process_scancode(uint8_t sc) {
    char c = scancode_to_ascii(sc);
    if (c == 0) return;  // modifier press/release, caps lock, etc.
    uint64_t irq;
    spin_lock_irqsave(&tty_lock, &irq);
    tty_t *t = &ttys[keyboard_tty];
    tcflag_t lflags = t->termios.c_lflag;
    cc_t vintr = t->termios.c_cc[VINTR];
    cc_t vsusp = t->termios.c_cc[VSUSP];
    int sig = 0;
    if ((lflags & ISIG) && vintr && c == (char)vintr) sig = SIGINT;
    else if ((lflags & ISIG) && vsusp && c == (char)vsusp) sig = SIGTSTP;
    if (sig) {
        t->pending_isig = sig;
        t->pending_isig_c = c;
    } else {
        write_tty_ring(&t->input, &c, 1);
    }
    spin_unlock_irqrestore(&tty_lock, irq);

    if (sig) tty_process_input_signals();
}

void tty_process_input_signals(void) {
    uint64_t irq;
    spin_lock_irqsave(&tty_lock, &irq);
    tty_t *t = &ttys[keyboard_tty];
    int sig = t->pending_isig;
    if (!sig) { spin_unlock_irqrestore(&tty_lock, irq); return; }
    char c = t->pending_isig_c;
    tcflag_t lflags = t->termios.c_lflag;
    t->pending_isig = 0;
    spin_unlock_irqrestore(&tty_lock, irq);

    int delivered = tty_signal_pgrp(keyboard_tty, sig);
    if (delivered == 0) {
        spin_lock_irqsave(&tty_lock, &irq);
        write_tty_ring(&t->input, &c, 1);
        spin_unlock_irqrestore(&tty_lock, irq);
        return;
    }

    if (lflags & ECHO) {
        if (sig == SIGINT) { printf("^C"); putchar('\n'); }
        else if (sig == SIGTSTP) { printf("^Z"); putchar('\n'); }
    }
}

int tty_signal_pgrp(int tty_idx, int sig) {
    if (sig < 1 || sig > 31) return 0;
    tty_t *t = get_tty(tty_idx);
    int delivered = 0;
    if (!t || t->fg_pgrp == 0) {
        // No foreground pgrp set: fall back to signalling current task
        if (current_task_ptr) {
            uint64_t handler = current_task_ptr->sigactions[sig * 4];
            if (handler != 1 /* SIG_IGN */) {
                current_task_ptr->pending_signals |= (1ULL << sig);
                delivered = 1;
            }
        }
        return delivered;
    }
    pid_t fpgrp = t->fg_pgrp;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) continue;
        if (tasks[i].ctty_idx != tty_idx) continue;
        if (tasks[i].pgid != fpgrp) continue;
        // Skip if the task is explicitly ignoring this signal
        uint64_t handler = tasks[i].sigactions[sig * 4];
        if (handler == 1 /* SIG_IGN */) continue;
        delivered++;
        // SIGTSTP/SIGSTOP with SIG_DFL stop the task directly
        if (sig == SIGTSTP || sig == SIGSTOP) {
            if (handler == 0 /* SIG_DFL */) {
                if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_READY) {
                    tasks[i].state = TASK_STOPPED;
                    tasks[i].stop_reported = 0;
                    // Notify parent so waitpid(WUNTRACED) wakes up
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
            // Wake a stopped/sleeping task so it can process the signal
            if (tasks[i].state == TASK_STOPPED)
                tasks[i].state = TASK_READY;
        }
    }
    return delivered;
}

void set_keyboard_tty(int tty_idx) {
    if (tty_idx >= 0 && tty_idx < NUM_TTYS) {
        keyboard_tty = tty_idx;
    }
}

void init_ttys(void) {
    for (int i = 0; i < NUM_TTYS; i++) {
        ttys[i].input.head = ttys[i].input.tail = 0;
        ttys[i].active = true;
        ttys[i].fg_pgrp = 0;
        ttys[i].pending_isig = 0;
        ttys[i].termios.c_iflag = 0x0500;
        ttys[i].termios.c_oflag = 0x0005;
        ttys[i].termios.c_cflag = 0x04BF;
        ttys[i].termios.c_lflag = 0x8A3B;
        // Default control characters
        ttys[i].termios.c_cc[VINTR]  = 0x03;  // Ctrl+C -> SIGINT
        ttys[i].termios.c_cc[VQUIT]  = 0x1C;  // Ctrl+\ -> SIGQUIT
        ttys[i].termios.c_cc[VERASE] = 0x7F;  // DEL (backspace)
        ttys[i].termios.c_cc[VKILL]  = 0x15;  // Ctrl+U
        ttys[i].termios.c_cc[VEOF]   = 0x04;  // Ctrl+D
        ttys[i].termios.c_cc[VSUSP]  = 0x1A;  // Ctrl+Z -> SIGTSTP
    }
}
