#pragma once

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10

typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} dirent_t;

