#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/spinlocks.h>
#include <io/devices.h>

#define MAX_DEVTMPFS_DEVICES 64

typedef struct {
    char name[65];
    uint64_t (*read)(void* buf, uint64_t count, uint64_t offset, int dev_idx);
    uint64_t (*write)(const void* buf, uint64_t count, uint64_t offset, int dev_idx);
    uint64_t size;
    bool active;
    bool block;
    int index; // Device-specific index (e.g., TTY number)
    disk_device_bus_t bus; // Backing bus of a whole-disk block device
} devtmpfs_device_t;

extern devtmpfs_device_t devtmpfs_devices[MAX_DEVTMPFS_DEVICES];
extern spinlock_t devtmpfs_lock;

bool device_exists_on_devtmpfs(const char* name);
const char *get_devtmpfs_device_name(int index);
bool is_devtmpfs_path(const char *path, char *rel_out);
