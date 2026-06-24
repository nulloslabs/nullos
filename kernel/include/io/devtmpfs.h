#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>
#include <main/spinlocks.h>

#define MAX_DEVTMPFS_DEVICES 64

typedef struct {
    char name[65];
    uint64_t (*read)(void* buf, uint64_t count, uint64_t offset, int dev_idx);
    uint64_t (*write)(const void* buf, uint64_t count, uint64_t offset, int dev_idx);
    bool active;
    int index; // Device-specific index (e.g., TTY number)
} devtmpfs_device_t;

extern devtmpfs_device_t devtmpfs_devices[MAX_DEVTMPFS_DEVICES];
extern spinlock_t devtmpfs_lock;

bool devtmpfs_device_exists(const char* name);
const char *devtmpfs_get_device_name(int index);
