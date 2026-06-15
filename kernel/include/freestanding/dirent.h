#pragma once

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} dirent_t;

struct __dirstream {
    int fd;
    int buf_pos;
    int buf_end;
    volatile int lock[1];
    char buf[2048];
};

typedef struct __dirstream DIR;
