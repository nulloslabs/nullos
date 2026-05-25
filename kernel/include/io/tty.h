#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <main/spinlock.h>
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
    termios_t  termios;
} tty_t;

tty_t *get_tty(int idx);
spinlock_t *get_tty_lock(void);
int read_tty_ring(tty_ring_t *r, char *buf, int len);
int write_tty_ring(tty_ring_t *r, const char *buf, int len);
int get_tty_ring_count(tty_ring_t *r);
void init_ttys(void);
