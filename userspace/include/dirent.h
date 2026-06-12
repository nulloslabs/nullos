#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} __attribute__((packed));

struct __dirstream {
    int fd;
    int buf_pos;
    int buf_end;
    volatile int lock[1];
    char buf[2048];
};

typedef struct __dirstream DIR;

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

int getdents(int fd, struct dirent *buf, int count);
DIR *opendir(const char *name);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif