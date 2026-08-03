#pragma once

#include <stdint.h>
#include <io/ide.h>

#define PATA_CLASS        0x01
#define PATA_SUBCLASS     0x01
#define PATA_PROGIF_MASK  0x80
#define PATA_PROGIF_VALUE 0x80

#define PATA_COMMAND_READ_DMA    0xC8
#define PATA_COMMAND_WRITE_DMA   0xCA
#define PATA_COMMAND_CACHE_FLUSH 0xE7
#define PATA_COMMAND_IDENTIFY    0xEC

#define PATA_SECTOR_SIZE     512
#define PATA_DMA_MAX_SECTORS (IDE_DMA_BUFFER_SIZE / PATA_SECTOR_SIZE)
#define PATA_LBA28_LIMIT     0x10000000ULL

int read_pata(void *data, uint64_t count, uint64_t offset);
int write_pata(const void *data, uint64_t count, uint64_t offset);
void init_pata(void);
