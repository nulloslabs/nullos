#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ATAPI_COMMAND_IDENTIFY_PACKET 0xA1
#define ATAPI_COMMAND_PACKET          0xA0
#define ATAPI_PACKET_READ_CAPACITY    0x25
#define ATAPI_PACKET_READ_12          0xA8

#define ATAPI_SECTOR_SIZE     2048
#define ATAPI_DMA_MAX_SECTORS (IDE_DMA_BUFFER_SIZE / ATAPI_SECTOR_SIZE)

int read_atapi(void *data, uint64_t count, uint64_t offset);
uint64_t read_atapi_device(void *data, uint64_t count, uint64_t offset, int index);
uint64_t write_atapi_device(const void *data, uint64_t count, uint64_t offset, int index);
bool atapi_device_size(int index, uint64_t *size);
void init_atapi(void);
