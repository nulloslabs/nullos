#pragma once

#include <freestanding/stdint.h>
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
    bool active;
    struct termios termios;
    pid_t fg_pgrp;  // foreground process group for this tty (0 = none)
    volatile int  pending_isig;    // SIGINT/SIGTSTP queued from keyboard ISR
    volatile char pending_isig_c;    // original control character
} tty_t;

extern spinlock_t tty_lock;

tty_t *get_tty(int idx);
int read_tty_ring(tty_ring_t *r, char *buf, int len);
int write_tty_ring(tty_ring_t *r, const char *buf, int len);
int get_tty_ring_count(tty_ring_t *r);
void tty_process_scancode(uint8_t sc);
int tty_signal_pgrp(int tty_idx, int sig);
void tty_process_input_signals(void);
void set_keyboard_tty(int tty_idx);
void init_ttys(void);
