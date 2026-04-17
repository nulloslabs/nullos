#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>

#define MAX_DEVFS_DEVICES 32

typedef struct {
    char name[64];
    uint64_t (*read)(void* buf, uint64_t count, uint64_t offset);
    uint64_t (*write)(const void* buf, uint64_t count, uint64_t offset);
    bool active;
} devfs_device_t;

void init_devfs(void);
int register_devfs_device(const char* name, 
    uint64_t (*read_fn)(void*, uint64_t, uint64_t), 
    uint64_t (*write_fn)(const void*, uint64_t, uint64_t));

uint64_t read_devfs(const char* name, void* buf, uint64_t count, uint64_t offset);
uint64_t write_devfs(const char* name, const void* buf, uint64_t count, uint64_t offset);
bool devfs_device_exists(const char* name);
const char *devfs_get_device_name(int index);
