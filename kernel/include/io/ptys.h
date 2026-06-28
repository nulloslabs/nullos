#pragma once

#include <freestanding/stdint.h>
#include <freestanding/termios.h>
#include <freestanding/sys/types.h>
#include <io/ttys.h>

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
extern spinlock_t pty_lock;

int alloc_pty(void);
void retain_pty_master(int idx);
void release_pty_master(int idx);
int open_pty_slave(int idx);
void retain_pty_slave(int idx);
void release_pty_slave(int idx);
int pty_slave_path_idx(const char *path);
pty_t *get_pty(int idx);
int read_pty_master(int idx, char *buf, int len);
int write_pty_master(int idx, const char *buf, int len);
uint64_t read_pts(int idx, void *buf, uint64_t count, uint64_t offset);
uint64_t write_pts(int idx, const void *buf, uint64_t count, uint64_t offset);
void init_ptys(void);
int pty_signal_pgrp(int pty_idx, int sig);