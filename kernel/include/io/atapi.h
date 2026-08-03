#pragma once

#include <stdint.h>
#include <io/ide.h>

#define ATAPI_CLASS        0x01
#define ATAPI_SUBCLASS     0x01
#define ATAPI_PROGIF_MASK  0x80
#define ATAPI_PROGIF_VALUE 0x80

#define ATAPI_COMMAND_IDENTIFY_PACKET 0xA1
#define ATAPI_COMMAND_PACKET          0xA0
#define ATAPI_PACKET_READ_CAPACITY    0x25
#define ATAPI_PACKET_READ_12          0xA8

#define ATAPI_SECTOR_SIZE     2048
#define ATAPI_DMA_MAX_SECTORS (IDE_DMA_BUFFER_SIZE / ATAPI_SECTOR_SIZE)

int read_atapi(void *data, uint64_t count, uint64_t offset);
void init_atapi(void);
