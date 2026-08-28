#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <io/devices.h>

#define GPT_SIGNATURE       0x5452415020494645ULL
#define GPT_MIN_HEADER_SIZE 92
#define GPT_MIN_ENTRY_SIZE  128
#define GPT_MAX_ENTRY_BYTES (4 * 1024 * 1024)
#define GPT_MAX_PARTITIONS  128

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entry_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entry_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed)) gpt_entry_t;

typedef struct {
    char name[24];
    int disk_index;
    uint64_t offset;
    uint64_t size;
    disk_device_bus_t bus;
    bool active;
} gpt_partition_t;

bool probe_gpt_for_sata_disk(int disk_index, const char *disk_name, uint64_t disk_size);
bool probe_gpt_for_pata_disk(int disk_index, const char *disk_name, uint64_t disk_size);
bool probe_gpt_for_nvme_disk(int disk_index, const char *disk_name, uint64_t disk_size);
void remove_gpt_partitions(int disk_index, disk_device_bus_t bus);

