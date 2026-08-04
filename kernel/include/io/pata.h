#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PATA_COMMAND_READ_DMA    0xC8
#define PATA_COMMAND_WRITE_DMA   0xCA
#define PATA_COMMAND_CACHE_FLUSH 0xE7
#define PATA_COMMAND_IDENTIFY    0xEC

#define PATA_SECTOR_SIZE     512
#define PATA_DMA_MAX_SECTORS (IDE_DMA_BUFFER_SIZE / PATA_SECTOR_SIZE)
#define PATA_LBA28_LIMIT     0x10000000ULL

int read_pata(void *data, uint64_t count, uint64_t offset);
int write_pata(const void *data, uint64_t count, uint64_t offset);
uint64_t read_pata_device(void *data, uint64_t count, uint64_t offset, int index);
uint64_t write_pata_device(const void *data, uint64_t count, uint64_t offset, int index);
bool pata_device_size(int index, uint64_t *size);
void init_pata(void);
