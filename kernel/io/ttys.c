#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <freestanding/signal.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <io/ttys.h>
#include <io/terminal.h>
#include <io/keyboard.h>
#include <io/ptys.h>

static tty_t ttys[NUM_TTYS];
spinlock_t tty_lock = SPINLOCK_INIT;

// Track which TTY receives keyboard input (no VT switching yet, so we use the last active TTY)
static int keyboard_tty = 0;
static int keyboard_pty = -1;

// NOTE: Another static function in syscall_impls.c is exactly named deliver_sig_to_task, watch out!
static void deliver_sig_to_task(int idx, int sig) {
    task_t *t = &tasks[idx];
    if (t->sigactions[sig * 4] == (uint64_t)SIG_IGN) return;

    if (sig == SIGTSTP || sig == SIGSTOP) {
        if (t->sigactions[sig * 4] == (uint64_t)SIG_DFL) {
            // Stop in place and notify the parent for waitpid(WUNTRACED).
            if (t->state == TASK_RUNNING || t->state == TASK_READY) {
                t->state = TASK_STOPPED;
                t->stopped_by_signal = 1;
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
        if (t->state == TASK_STOPPED) t->state = TASK_READY;
    }
}

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

tty_t *get_tty(int idx) {
    if (idx < 0 || idx >= NUM_TTYS) return NULL;
    return &ttys[idx];
}

// Write a string directly into the active TTY's input ring.
// Used to inject multi-byte ANSI escape sequences for special keys.
static void write_tty_input_str(const char *s) {
    if (keyboard_pty >= 0) {
        write_tty_ring(&ptys[keyboard_pty].s2m, s, (int)strlen(s));
    } else {
        tty_t *t = &ttys[keyboard_tty];
        while (*s) {
            write_tty_ring(&t->input, s, 1);
            s++;
        }
    }
}

// State for 0xE0 extended scancode prefix (PS/2 set 1).
static bool extended_pending = false;

void tty_process_scancode(uint8_t sc) {
    // --- Alt key tracking ---
    if (sc == 0x38) return;   // Alt press - already tracked in keyboard.c
    if (sc == 0xB8) return;   // Alt release - already tracked in keyboard.c

    // --- 0xE0 extended prefix (PS/2 arrow/nav keys) ---
    if (sc == 0xE0) {
        extended_pending = true;
        return;
    }

    // Extended key: second byte after 0xE0 prefix
    if (extended_pending) {
        extended_pending = false;
        // Ignore extended releases (0x48|0x80=0xC8, etc.)
        if (sc & 0x80) return;
        // Map PS/2 extended scancode -> ANSI escape sequence
        const char *seq = NULL;
        switch (sc) {
            case 0x48: seq = "\033[A"; break;  // Up
            case 0x50: seq = "\033[B"; break;  // Down
            case 0x4D: seq = "\033[C"; break;  // Right
            case 0x4B: seq = "\033[D"; break;  // Left
            case 0x47: seq = "\033[H"; break;  // Home
            case 0x4F: seq = "\033[F"; break;  // End
            case 0x49: seq = "\033[5~"; break; // Page Up
            case 0x51: seq = "\033[6~"; break; // Page Down
            case 0x52: seq = "\033[2~"; break; // Insert
            case 0x53: seq = "\033[3~"; break; // Delete
            default: return;  // Unknown extended key, ignore
        }

        uint64_t irq;
        spin_lock_irqsave(&tty_lock, &irq);
        write_tty_input_str(seq);
        spin_unlock_irqrestore(&tty_lock, irq);
        return;
    }

    // --- Virtual scancodes (0x60-0x6F) for USB HID special keys ---
    // The USB keyboard driver maps arrow/navigation HID codes to these
    // virtual scancodes so they arrive without a 0xE0 prefix.
    if (sc >= 0x60 && sc <= 0x6F && !(sc & 0x80)) {
        const char *vseq = NULL;
        switch (sc) {
            case 0x60: vseq = "\033[A"; break;  // Up
            case 0x61: vseq = "\033[B"; break;  // Down
            case 0x62: vseq = "\033[C"; break;  // Right
            case 0x63: vseq = "\033[D"; break;  // Left
            case 0x64: vseq = "\033[H"; break;  // Home
            case 0x65: vseq = "\033[F"; break;  // End
            case 0x66: vseq = "\033[5~"; break; // Page Up
            case 0x67: vseq = "\033[6~"; break; // Page Down
            case 0x68: vseq = "\033[2~"; break; // Insert
            case 0x69: vseq = "\033[3~"; break; // Delete
        }
        if (vseq) {
            uint64_t irq;
            spin_lock_irqsave(&tty_lock, &irq);
            write_tty_input_str(vseq);
            spin_unlock_irqrestore(&tty_lock, irq);
        }
        return;
    }

    // --- F-keys (PS/2 set 1, non-extended scancodes) ---
    // These need multi-byte ANSI sequences. Handle before scancode_to_ascii
    // since the lower[] table would return garbage for these scancodes.
    {
        const char *fseq = NULL;
        switch (sc) {
            case 0x3B: fseq = "\033OP";  break; // F1
            case 0x3C: fseq = "\033OQ";  break; // F2
            case 0x3D: fseq = "\033OR";  break; // F3
            case 0x3E: fseq = "\033OS";  break; // F4
            case 0x3F: fseq = "\033[15~"; break; // F5
            case 0x40: fseq = "\033[17~"; break; // F6
            case 0x41: fseq = "\033[18~"; break; // F7
            case 0x42: fseq = "\033[19~"; break; // F8
            case 0x43: fseq = "\033[20~"; break; // F9
            case 0x44: fseq = "\033[21~"; break; // F10
            case 0x57: fseq = "\033[23~"; break; // F11
            case 0x58: fseq = "\033[24~"; break; // F12
        }
        if (fseq) {
            uint64_t irq;
            spin_lock_irqsave(&tty_lock, &irq);
            write_tty_input_str(fseq);
            spin_unlock_irqrestore(&tty_lock, irq);
            return;
        }
    }

    // --- Regular key: convert scancode to ASCII ---
    char c = scancode_to_ascii(sc);
    if (c == 0) return;  // modifier press/release, caps lock, etc.

    // --- Alt+key: prefix with ESC (vim-style meta) ---
    // keyboard.c tracks alt_pressed; we read it via extern below.
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

    if (keyboard_pty >= 0) {
        // Keyboard input goes to the PTY master's s2m ring (simulates a
        // terminal emulator writing to the master side).
        if (sig) {
            pty_signal_pgrp(keyboard_pty, sig);
            if (!(lflags & NOFLSH)) {
                ptys[keyboard_pty].m2s.head = ptys[keyboard_pty].m2s.tail = 0;
            }
            echo = (lflags & ECHO) != 0;
        } else {
            if (kbd_alt_pressed()) {
                char esc = '\033';
                write_tty_ring(&ptys[keyboard_pty].s2m, &esc, 1);
            }
            write_tty_ring(&ptys[keyboard_pty].s2m, &c, 1);
            echo = (lflags & ECHO) != 0;
        }
    } else if (sig) {
        tty_signal_pgrp(keyboard_tty, sig);
        if (!(lflags & NOFLSH)) {
            t->input.head = t->input.tail = 0;
        }
        echo = (lflags & ECHO) != 0;
    } else {
        // Alt+key: emit ESC prefix first (for apps like vi that use ESC as meta)
        if (kbd_alt_pressed()) {
            char esc = '\033';
            write_tty_ring(&t->input, &esc, 1);
        }
        write_tty_ring(&t->input, &c, 1);
    }
    spin_unlock_irqrestore(&tty_lock, irq);

    if (echo) {
        if (sig == SIGINT) printf("^C");
        else if (sig == SIGTSTP) printf("^Z");
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
            if (tasks[i].sigactions[sig * 4] == (uint64_t)SIG_IGN) continue;
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
        if (tasks[i].sigactions[sig * 4] == (uint64_t)SIG_IGN) continue;
        deliver_sig_to_task(i, sig);
        delivered++;
    }
    return delivered;
}

void set_keyboard_tty(int tty_idx) {
    if (tty_idx >= 0 && tty_idx < NUM_TTYS) {
        keyboard_tty = tty_idx;
        keyboard_pty = -1;  // Switching to a real TTY disables PTY keyboard
    }
}

void set_keyboard_pty(int pty_idx) {
    if (pty_idx >= 0 && pty_idx < NUM_PTYS) {
        keyboard_pty = pty_idx;
    }
}

void clear_keyboard_pty(int pty_idx) {
    if (keyboard_pty == pty_idx) {
        keyboard_pty = -1;
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
        ttys[i].termios.c_cc[VINTR]  = 0x03;
        ttys[i].termios.c_cc[VQUIT]  = 0x1C;
        ttys[i].termios.c_cc[VERASE] = 0x7F;
        ttys[i].termios.c_cc[VKILL]  = 0x15;
        ttys[i].termios.c_cc[VEOF]   = 0x04;
        ttys[i].termios.c_cc[VSUSP]  = 0x1A;
    }
}
