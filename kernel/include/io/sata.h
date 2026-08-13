#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SATA_SECTOR_SIZE 512
#define SATA_MAX_DEVICES 8

extern bool is_sata_present;

int sata_device_count(void);
bool sata_device_size(int index, uint64_t *size);
bool make_sata_disk_name(char *name, uint64_t name_size, int index);
uint64_t read_sata_device(void *data, uint64_t count, uint64_t offset, int index);
uint64_t write_sata_device(const void *data, uint64_t count, uint64_t offset, int index);
void init_sata(void);
