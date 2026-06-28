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
    int echo = 0;


    if (sig) {
        // Deliver the signal to the foreground process group right away.
        // tty_signal_pgrp() only flips pending-signal bits (and may stop or
        // wake tasks) — it never sleeps or allocates — so it is safe to run
        // from this keyboard ISR.  Doing the delivery here, instead of
        // deferring it to the next syscall boundary, means a Ctrl+C interrupts
        // the foreground program the instant the key is pressed, regardless of
        // whether it happens to be blocked in read().
        tty_signal_pgrp(keyboard_tty, sig);

        // Flush the input ring buffer so partially-typed input is discarded.
        // This matches Linux's __isig() behaviour (unless NOFLSH is set).
        // Without this, stale characters leak into the next read() after the
        // signal handler returns or a new foreground program starts.
        if (!(lflags & NOFLSH)) {
            t->input.head = t->input.tail = 0;
        }

        echo = (lflags & ECHO) != 0;
    } else {
        write_tty_ring(&t->input, &c, 1);
    }
    spin_unlock_irqrestore(&tty_lock, irq);

    if (echo) {
        // Echo "^C"/"^Z" plus a newline directly so it appears immediately
        // (term_lock is irq-save, hence safe from this ISR).  Echoing here
        // rather than at a syscall boundary is what makes the visual feedback
        // match the keypress for every program, not just the one in read().
        if (sig == SIGINT) printf("^C\n");
        else               printf("^Z\n");
    }
}

/*
 * Deliver an ISIG-triggered signal (SIGINT/SIGTSTP/...) to the foreground
 * job of `tty_idx`. Called from the keyboard ISR (tty_process_scancode).
 *
 * Two delivery modes:
 *
 *   1. fg_pgrp > 0  (POSIX):  deliver to every member of the foreground
 *      process group. We deliberately do NOT require ctty_idx to match
 *      tty_idx here. After busybox getty/login do setsid(), their
 *      descendants inherit ctty_idx = -1 and may never re-acquire the tty
 *      via TIOCSCTTY; the old `ctty_idx != tty_idx` filter would then
 *      silently eat the SIGINT for the ENTIRE foreground job, which is
 *      exactly why "Ctrl+C does nothing until Enter": the only thing that
 *      ever woke the cooked-mode reader was '\n' landing in the ring.
 *
 *   2. fg_pgrp == 0 (no fg pgrp registered): the OLD code pended the signal
 *      on current_task_ptr, which at IRQ time is whoever happened to be
 *      running (often the idle task) — i.e. the SIGINT bit landed on the
 *      wrong task and never reached the program actually blocked in read().
 *      We now do best-effort delivery to every live task that still
 *      believes it owns tty_idx as its controlling terminal. Not strict
 *      POSIX (POSIX says "no fg group => don't deliver"), but it makes
 *      interactive Ctrl+C behave intuitively when the shell's tcsetpgrp
 *      couldn't run/complete.
 *
 * Note: SIGTTIN/SIGTTOU gating in read()/write() already enforces
 * background-vs-foreground at the syscall layer, so widening matching
 * here is safe.
 */
static void deliver_sig_to_task(int idx, int sig) {
    task_t *t = &tasks[idx];
    if (t->sigactions[sig * 4] == 1 /* SIG_IGN */) return;

    if (sig == SIGTSTP || sig == SIGSTOP) {
        if (t->sigactions[sig * 4] == 0 /* SIG_DFL */) {
            // Stop in place and notify the parent for waitpid(WUNTRACED).
            if (t->state == TASK_RUNNING || t->state == TASK_READY) {
                t->state = TASK_STOPPED;
                t->stop_reported = 0;
                for (int j = 0; j < MAX_TASKS; j++) {
                    if (tasks[j].state != TASK_DEAD && tasks[j].pid == t->ppid) {
                        tasks[j].pending_signals |= (1ULL << SIGCHLD);
                        break;
                    }
                }
            }
        } else {
            // Installed handler: just pend; handler runs at next syscall.
            t->pending_signals |= (1ULL << sig);
        }
    } else {
        t->pending_signals |= (1ULL << sig);
        // Wake a stopped task so it can run its handler / be terminated.
        if (t->state == TASK_STOPPED)
            t->state = TASK_READY;
    }
}

int tty_signal_pgrp(int tty_idx, int sig) {
    if (sig < 1 || sig > 31) return 0;
    tty_t *t = get_tty(tty_idx);
    if (!t) return 0;

    int delivered = 0;
    pid_t fpgrp = t->fg_pgrp;

    if (fpgrp > 0) {
        // Mode 1: foreground process group only (see big comment above).
        // Filter purely on pgid; do NOT filter on ctty_idx.
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_DEAD) continue;
            if (tasks[i].pgid != fpgrp) continue;
            if (tasks[i].sigactions[sig * 4] == 1 /* SIG_IGN */) continue;
            deliver_sig_to_task(i, sig);
            delivered++;
        }
        return delivered;
    }

    // Mode 2: no fg pgrp registered. Best-effort delivery to everything on
    // this controlling tty. Replaces the old incorrect
    // "signal current_task_ptr" fallback.
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) continue;
        if (tasks[i].ctty_idx != tty_idx) continue;
        if (tasks[i].sigactions[sig * 4] == 1 /* SIG_IGN */) continue;
        deliver_sig_to_task(i, sig);
        delivered++;
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
