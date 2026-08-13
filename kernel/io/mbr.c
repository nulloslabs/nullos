#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <io/devices.h>
#include <io/mbr.h>
#include <io/sata.h>

static mbr_partition_t mbr_partitions[MBR_MAX_PARTITIONS];
static int mbr_partition_count;

static bool mbr_is_extended(uint8_t type) { return type == 0x05 || type == 0x0F || type == 0x85; }

static bool mbr_partition_bounds(uint64_t first_lba, uint64_t sectors, uint64_t disk_size, uint64_t *offset, uint64_t *size) {
    uint64_t disk_sectors = disk_size / SATA_SECTOR_SIZE;
    if (!sectors || first_lba >= disk_sectors || sectors > disk_sectors - first_lba) return false;
    *offset = first_lba * SATA_SECTOR_SIZE;
    *size = sectors * SATA_SECTOR_SIZE;
    return true;
}

static bool mbr_make_partition_name(char *name, uint64_t name_size, const char *disk_name, int number) {
    uint64_t length = strlen(disk_name);
    char digits[12];
    uint64_t digit_count = 0;
    do {
        digits[digit_count++] = '0' + number % 10;
        number /= 10;
    } while (number);
    if (length + digit_count + 1 > name_size) return false;
    memcpy(name, disk_name, length);
    for (uint64_t i = 0; i < digit_count; i++) name[length + i] = digits[digit_count - i - 1];
    name[length + digit_count] = '\0';
    return true;
}

static uint64_t read_mbr_partition(void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= mbr_partition_count) return (uint64_t)-ENODEV;
    mbr_partition_t *partition = &mbr_partitions[index];
    if (offset >= partition->size) return 0;
    if (count > partition->size - offset) count = partition->size - offset;
    return read_sata_device(data, count, partition->offset + offset, partition->disk_index);
}

static uint64_t write_mbr_partition(const void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= mbr_partition_count) return (uint64_t)-ENODEV;
    mbr_partition_t *partition = &mbr_partitions[index];
    if (offset >= partition->size) return (uint64_t)-ENOSPC;
    if (count > partition->size - offset) count = partition->size - offset;
    return write_sata_device(data, count, partition->offset + offset, partition->disk_index);
}

static bool mbr_register_partition(int disk_index, const char *disk_name, int number, uint64_t first_lba, uint64_t sectors, uint64_t disk_size) {
    if (mbr_partition_count >= MBR_MAX_PARTITIONS) return false;
    uint64_t offset;
    uint64_t size;
    if (!mbr_partition_bounds(first_lba, sectors, disk_size, &offset, &size)) return false;
    char name[24];
    if (!mbr_make_partition_name(name, sizeof(name), disk_name, number)) return false;
    int index = mbr_partition_count;
    mbr_partitions[index].disk_index = disk_index;
    mbr_partitions[index].offset = offset;
    mbr_partitions[index].size = size;
    if (register_block_device_idx(name, read_mbr_partition, write_mbr_partition, index, size) < 0) return false;
    mbr_partition_count++;
    log("mbr: registered %s, lba %llu, %llu sectors\n", name, first_lba, sectors);
    return true;
}

static bool mbr_scan_extended(int disk_index, const char *disk_name, uint64_t base_lba, uint64_t disk_size) {
    uint64_t ebr_lba = base_lba;
    uint64_t visited[MBR_MAX_EBR_CHAIN];
    int visited_count = 0;
    int partition_number = 5;
    bool found = false;
    for (int chain = 0; chain < MBR_MAX_EBR_CHAIN; chain++) {
        bool duplicate = false;
        for (int i = 0; i < visited_count; i++) if (visited[i] == ebr_lba) duplicate = true;
        if (duplicate || ebr_lba >= disk_size / SATA_SECTOR_SIZE) return found;
        visited[visited_count++] = ebr_lba;
        uint8_t sector[SATA_SECTOR_SIZE];
        uint64_t result = read_sata_device(sector, sizeof(sector), ebr_lba * SATA_SECTOR_SIZE, disk_index);
        if (result != sizeof(sector) || *(uint16_t *)(sector + 510) != MBR_SIGNATURE) return found;
        mbr_entry_t *entries = (mbr_entry_t *)(sector + MBR_PARTITION_OFFSET);
        if (entries[0].type && !mbr_is_extended(entries[0].type)) {
            uint64_t logical_lba = ebr_lba + entries[0].first_lba;
            if (logical_lba >= ebr_lba && mbr_register_partition(disk_index, disk_name, partition_number, logical_lba, entries[0].sectors, disk_size)) found = true;
            partition_number++;
        }
        if (!mbr_is_extended(entries[1].type) || !entries[1].sectors) return found;
        uint64_t next_lba = base_lba + entries[1].first_lba;
        if (next_lba < base_lba) return found;
        ebr_lba = next_lba;
    }
    return found;
}

bool mbr_probe_sata_disk(int disk_index, const char *disk_name, uint64_t disk_size) {
    if (!disk_name || disk_size < SATA_SECTOR_SIZE) return false;
    uint8_t sector[SATA_SECTOR_SIZE];
    if (read_sata_device(sector, sizeof(sector), 0, disk_index) != sizeof(sector)) return false;
    if (*(uint16_t *)(sector + 510) != MBR_SIGNATURE) return false;
    mbr_entry_t *entries = (mbr_entry_t *)(sector + MBR_PARTITION_OFFSET);
    bool found = false;
    int partition_number = 1;
    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        if (!entries[i].type || !entries[i].sectors || entries[i].type == 0xEE) {
            partition_number++;
            continue;
        }
        if (mbr_is_extended(entries[i].type)) {
            if (mbr_scan_extended(disk_index, disk_name, entries[i].first_lba, disk_size)) found = true;
            partition_number++;
            continue;
        }
        if (mbr_register_partition(disk_index, disk_name, partition_number, entries[i].first_lba, entries[i].sectors, disk_size)) found = true;
        partition_number++;
    }
    return found;
}
