#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/sys/types.h>
#include <main/spinlocks.h>
#include <freestanding/termios.h>

#define NUM_TTYS     8
#define TTY_BUF_SIZE 4096

typedef struct {
    char     buf[TTY_BUF_SIZE];
    uint32_t head, tail;
} tty_ring_t;

typedef struct {
    tty_ring_t input;
    bool       active;
    struct termios termios;
    pid_t      fg_pgrp;  // foreground process group for this tty (0 = none)
    volatile int  pending_isig;    // SIGINT/SIGTSTP queued from keyboard ISR
    volatile char pending_isig_c;    // original control character
} tty_t;

tty_t *get_tty(int idx);
spinlock_t *get_tty_lock(void);
int read_tty_ring(tty_ring_t *r, char *buf, int len);
int write_tty_ring(tty_ring_t *r, const char *buf, int len);
int get_tty_ring_count(tty_ring_t *r);
void init_ttys(void);

// Feed a raw scancode into the active TTY's input ring buffer after
// converting it to an ASCII character (shift, caps, ctrl handled).
// Called from keyboard ISRs so must be safe in interrupt context.
// Currently always writes to tty0 (no VT switching).
void tty_process_scancode(uint8_t sc);

// Deliver signal `sig` to every non-dead task whose ctty_idx matches
// the tty index `tty_idx` and whose pgid matches tty->fg_pgrp.
// Tasks with SIG_IGN disposition for `sig` are skipped.
// Returns the number of tasks that received the signal.
// Safe to call from syscall context (not from IRQ).
int tty_signal_pgrp(int tty_idx, int sig);

// Deliver a control character queued by tty_process_scancode from IRQ
// context.  Must be called from syscall context before dispatch.
void tty_process_input_signals(void);
