#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/spinlocks.h>

#define MAX_DEVTMPFS_DEVICES 64

typedef enum {
    DEV_BUS_NONE = 0,
    DEV_BUS_PATA,
    DEV_BUS_SATA,
} device_bus_t;

typedef struct {
    char name[65];
    uint64_t (*read)(void* buf, uint64_t count, uint64_t offset, int dev_idx);
    uint64_t (*write)(const void* buf, uint64_t count, uint64_t offset, int dev_idx);
    uint64_t size;
    bool active;
    bool block;
    int index; // Device-specific index (e.g., TTY number)
    device_bus_t bus; // Backing bus of a whole-disk block device
} devtmpfs_device_t;

extern devtmpfs_device_t devtmpfs_devices[MAX_DEVTMPFS_DEVICES];
extern spinlock_t devtmpfs_lock;

bool device_exists_on_devtmpfs(const char* name);
const char *get_devtmpfs_device_name(int index);
bool is_devtmpfs_path(const char *path, char *rel_out);
