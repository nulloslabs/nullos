#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

#define DIRENT64_HEADER_SIZE ((uint16_t)sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint8_t))
#define DIRENT64_ALIGN(n) (((n) + 7) & ~7)

struct dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

typedef struct {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} linux_dirent64_t;

struct __dirstream {
    int fd;
    int buf_pos;
    int buf_end;
    volatile int lock[1];
    char buf[2048];
};

typedef struct __dirstream DIR;

int getdents(int fd, struct dirent *buf, int count);
DIR *opendir(const char *name);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif
