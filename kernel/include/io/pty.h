#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <termios.h>
#include <sys/types.h>
#include <io/tty.h>

#define NUM_PTYS 16

typedef struct {
    tty_ring_t m2s;     // master writes → slave reads
    tty_ring_t s2m;     // slave writes  → master reads
    int master_refs;
    int slave_refs;
    bool allocated;
    bool locked;
    struct termios termios;
    pid_t fg_pgrp;
} pty_t;

extern pty_t ptys[NUM_PTYS];
extern int keyboard_pty;
extern spinlock_t pty_lock;

int alloc_pty(void);
void destroy_pty(int idx);
void retain_pty_master(int idx);
void release_pty_master(int idx);
int open_pty_slave(int idx);
void retain_pty_slave(int idx);
void release_pty_slave(int idx);
int pty_slave_path_idx(const char *path);
pty_t *get_pty(int idx);
int signal_pty_pgrp(int pty_idx, int sig);
int read_pty_master(int idx, char *buf, int len);
int write_pty_master(int idx, const char *buf, int len);
void set_keyboard_pty(int pty_idx);
void clear_keyboard_pty(int pty_idx);
void init_pty(void);
