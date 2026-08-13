#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MBR_SIGNATURE         0xAA55
#define MBR_PARTITION_OFFSET  446
#define MBR_PARTITION_COUNT   4
#define MBR_MAX_PARTITIONS    32
#define MBR_MAX_EBR_CHAIN     128

typedef struct {
    uint8_t status;
    uint8_t first_chs[3];
    uint8_t type;
    uint8_t last_chs[3];
    uint32_t first_lba;
    uint32_t sectors;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    int disk_index;
    uint64_t offset;
    uint64_t size;
} mbr_partition_t;

bool mbr_probe_sata_disk(int disk_index, const char *disk_name, uint64_t disk_size);

