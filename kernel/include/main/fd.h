#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>

#define FD_MAX 32

// File descriptor types
typedef enum {
    FD_NONE = 0,
    FD_FILE = 1,  // rootfs file
    FD_STREAM = 2,  // std(in/out/err) device
    FD_DEV = 3,   // char device
} fd_type_t;

typedef struct {
    bool open;
    fd_type_t type;
    char path[256];
    uint64_t offset;   // current read/write position
    uint32_t flags;    // O_RDONLY, O_WRONLY, O_RDWR
} fd_entry_t;

typedef struct {
    fd_entry_t entries[FD_MAX];
} fd_table_t;

void init_fd_table(fd_table_t *table);
int alloc_fd(fd_table_t *table, const char *path, fd_type_t type, uint32_t flags);
int free_fd(fd_table_t *table, int fd);
fd_entry_t *get_fd(fd_table_t *table, int fd);
fd_entry_t *get_current_fd(int fd);