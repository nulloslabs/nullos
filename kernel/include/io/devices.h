#pragma once

#include <stdint.h>
#include <sys/types.h>

typedef enum {
    DISK_BUS_NONE = 0,
    DISK_BUS_PATA,
    DISK_BUS_SATA,
    DISK_BUS_NVME,
} disk_device_bus_t;

// Compatibility aliases
typedef disk_device_bus_t device_bus_t;
#define DEV_BUS_NONE DISK_BUS_NONE
#define DEV_BUS_PATA DISK_BUS_PATA
#define DEV_BUS_SATA DISK_BUS_SATA
#define DEV_BUS_NVME DISK_BUS_NVME

uint64_t read_device(const char* name, void* buf, uint64_t count, uint64_t offset);
uint64_t write_device(const char* name, const void* buf, uint64_t count, uint64_t offset);
void init_devices(void);
int register_device(const char* name, uint64_t (*read_fn)(void*, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void*, uint64_t, uint64_t, int));
int register_device_idx(const char* name, uint64_t (*read_fn)(void*, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void*, uint64_t, uint64_t, int), int index);
int register_block_device_idx(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int), int index, uint64_t size);
int register_disk_device_idx(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int), int index, uint64_t size, disk_device_bus_t bus);
int get_block_device_size(const char *name, uint64_t *size);
int get_block_device_bus(const char *name, disk_device_bus_t *bus, int *disk_index);
int get_device_mode(const char *name, mode_t *mode);
int unregister_device(const char* name);
bool is_device_path(const char *path, char *rel_out, const char **fs_type_out);

// Backward compatibility: devices.h previously included devtmpfs.h
#include <io/devtmpfs.h>
