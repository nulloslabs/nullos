#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <io/tty.h>

#define NUM_PTYS 16

typedef struct {
    tty_ring_t m2s;     // master writes → slave reads
    tty_ring_t s2m;     // slave writes  → master reads
    bool allocated;
    bool locked;
} pty_t;

extern pty_t ptys[NUM_PTYS];
extern spinlock_t pty_lock;

int alloc_pty(void);
void free_pty(int idx);
pty_t *get_pty(int idx);
int read_pty_master(int idx, char *buf, int len);
int write_pty_master(int idx, const char *buf, int len);
void init_ptys(void);
