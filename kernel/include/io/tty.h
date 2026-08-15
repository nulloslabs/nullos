#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <main/spinlocks.h>
#include <termios.h>

#define NUM_TTYS     8
#define TTY_BUF_SIZE 4096
#define TTY_ACTIVE_INDEX -1
#define TTY_CTTY_INDEX -2
#define TTY_KEYMAP_TABLES 16
#define TTY_KEYMAP_KEYS 128
#define KBD_KEY_HOLE 0xF200
#define KBD_KEY_CONSOLE 0xF500

typedef struct {
    char     buf[TTY_BUF_SIZE];
    uint32_t head, tail;
} tty_ring_t;

typedef struct {
    tty_ring_t input;
    bool active;
    struct termios termios;
    pid_t fg_pgrp;  // foreground process group for this tty (0 = none)
} tty_t;

extern tty_t ttys[NUM_TTYS];
extern int keyboard_tty;
extern spinlock_t tty_lock;

tty_t *get_tty(int idx);
int read_tty_ring(tty_ring_t *r, char *buf, int len);
int write_tty_ring(tty_ring_t *r, const char *buf, int len);
int get_tty_ring_count(tty_ring_t *r);
void tty_process_scancode(uint8_t sc);
int signal_tty_pgrp(int tty_idx, int sig);
void set_keyboard_tty(int tty_idx);
uint16_t get_tty_keymap(int table, int key);
int set_tty_keymap(int table, int key, uint16_t value);
void init_tty(void);
