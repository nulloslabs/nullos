#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <main/signal.h>
#include <io/tty.h>
#include <io/keyboard.h>
#include <io/pty.h>
#include <io/terminal.h>

tty_t ttys[NUM_TTYS];
int keyboard_tty = 1;
spinlock_t tty_lock = SPINLOCK_INIT;

static bool extended_pending = false;
static uint16_t tty_keymap[TTY_KEYMAP_TABLES][TTY_KEYMAP_KEYS];

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

uint16_t get_tty_keymap(int table, int key) {
    if (table < 0 || table >= TTY_KEYMAP_TABLES || key < 0 || key >= TTY_KEYMAP_KEYS) return KBD_KEY_HOLE;
    return tty_keymap[table][key];
}

int set_tty_keymap(int table, int key, uint16_t value) {
    if (table < 0 || table >= TTY_KEYMAP_TABLES || key < 0 || key >= TTY_KEYMAP_KEYS) return -1;
    tty_keymap[table][key] = value;
    return 0;
}

void tty_process_scancode(uint8_t sc) {
    handle_keyboard_cad_scancode(sc);
    // --- Alt key tracking ---
    if (sc == 0x38 || sc == 0xB8) { (void)scancode_to_ascii(sc); return; }

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
        int table = (kbd_alt_pressed() ? 8 : 0) | (kbd_ctrl_pressed() ? 4 : 0);
        uint16_t keymap_value = get_tty_keymap(table, sc);
        if ((keymap_value & 0xFF00) == KBD_KEY_CONSOLE && (keymap_value & 0xFF) < NUM_TTYS - 1) { set_keyboard_tty((keymap_value & 0xFF) + 1); return; }
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
            signal_pty_pgrp(keyboard_pty, sig);
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
        signal_tty_pgrp(keyboard_tty, sig);
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
        if (sig == SIGINT) puts("^C");
        else if (sig == SIGTSTP) puts("^Z");
    }
}

int signal_tty_pgrp(int tty_idx, int sig) {
    if (sig < 1 || sig > 31) return 0;
    tty_t *t = get_tty(tty_idx);
    if (!t) return 0;

    int delivered = 0;
    pid_t fpgrp = t->fg_pgrp;

    if (fpgrp > 0) {
        // Mode 1: foreground process group only (see big comment above).
        // Filter purely on pgid; do NOT filter on ctty_idx.
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state == TASK_DEAD) continue;
            if (tasks[i]->pgid != fpgrp) continue;
            send_task_signal(i, sig);
            delivered++;
        }
        return delivered;
    }

    // Mode 2: no fg pgrp registered. Best-effort delivery to everything on
    // this controlling tty. Replaces the old incorrect
    // "signal current_task_ptr" fallback.
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->ctty_idx != tty_idx) continue;
        send_task_signal(i, sig);
        delivered++;
    }
    return delivered;
}

void set_keyboard_tty(int tty_idx) {
    if (tty_idx >= 0 && tty_idx < NUM_TTYS) {
        keyboard_tty = tty_idx;
        keyboard_pty = -1;  // Switching to a real TTY disables PTY keyboard
        switch_terminal_tty(tty_idx);
    }
}

void init_tty(void) {
    for (int table = 0; table < TTY_KEYMAP_TABLES; table++) for (int key = 0; key < TTY_KEYMAP_KEYS; key++) tty_keymap[table][key] = KBD_KEY_HOLE;
    for (int tty_idx = 1; tty_idx < NUM_TTYS; tty_idx++) tty_keymap[8][0x3A + tty_idx] = KBD_KEY_CONSOLE | (tty_idx - 1);
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
    log("tty: initialized tty\n");
}
