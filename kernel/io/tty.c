#include <freestanding/stddef.h>
#include <main/spinlock.h>
#include <io/tty.h>

static tty_t ttys[NUM_TTYS];
static spinlock_t tty_lock = SPINLOCK_INIT;

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

spinlock_t *get_tty_lock(void) { return &tty_lock; }

void init_ttys(void) {
    for (int i = 0; i < NUM_TTYS; i++) {
        ttys[i].input.head = ttys[i].input.tail = 0;
        ttys[i].active = true;
        ttys[i].termios.c_iflag = 0x0500;
        ttys[i].termios.c_oflag = 0x0005;
        ttys[i].termios.c_cflag = 0x04BF;
        ttys[i].termios.c_lflag = 0x8A3B;
        ttys[i].termios.c_cc[4] = 1;
    }
}