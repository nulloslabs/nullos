#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <mm/mm.h>
#include <io/devices.h>
#include <io/gpt.h>
#include <io/sata.h>

static gpt_partition_t gpt_partitions[GPT_MAX_PARTITIONS];
static int gpt_partition_count;

static uint32_t gpt_crc32(const void *data, uint64_t size) {
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (uint64_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
    return ~crc;
}

static bool gpt_guid_empty(const uint8_t guid[16]) {
    for (int i = 0; i < 16; i++) if (guid[i]) return false;
    return true;
}

static bool gpt_make_partition_name(char *name, uint64_t name_size, const char *disk_name, int number) {
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

static uint64_t read_gpt_partition(void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= gpt_partition_count) return (uint64_t)-ENODEV;
    gpt_partition_t *partition = &gpt_partitions[index];
    if (offset >= partition->size) return 0;
    if (count > partition->size - offset) count = partition->size - offset;
    return read_sata_device(data, count, partition->offset + offset, partition->disk_index);
}

static uint64_t write_gpt_partition(const void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= gpt_partition_count) return (uint64_t)-ENODEV;
    gpt_partition_t *partition = &gpt_partitions[index];
    if (offset >= partition->size) return (uint64_t)-ENOSPC;
    if (count > partition->size - offset) count = partition->size - offset;
    return write_sata_device(data, count, partition->offset + offset, partition->disk_index);
}

static bool gpt_read_header(int disk_index, uint64_t header_lba, uint64_t disk_sectors, gpt_header_t *header) {
    uint8_t sector[SATA_SECTOR_SIZE];
    if (header_lba >= disk_sectors) return false;
    if (read_sata_device(sector, sizeof(sector), header_lba * SATA_SECTOR_SIZE, disk_index) != sizeof(sector)) return false;
    memcpy(header, sector, sizeof(*header));
    if (header->signature != GPT_SIGNATURE || header->header_size < GPT_MIN_HEADER_SIZE || header->header_size > SATA_SECTOR_SIZE) return false;
    if (header->current_lba != header_lba || header->backup_lba >= disk_sectors || header->first_usable_lba > header->last_usable_lba || header->last_usable_lba >= disk_sectors) return false;
    uint32_t expected_crc = header->header_crc32;
    ((gpt_header_t *)sector)->header_crc32 = 0;
    if (gpt_crc32(sector, header->header_size) != expected_crc) return false;
    return true;
}

static uint8_t *gpt_read_entries(int disk_index, uint64_t disk_sectors, const gpt_header_t *header, uint64_t *entries_size) {
    if (header->entry_size < GPT_MIN_ENTRY_SIZE || header->entry_size % 8 || !header->entry_count) return NULL;
    if (header->entry_count > UINT64_MAX / header->entry_size) return NULL;
    *entries_size = (uint64_t)header->entry_count * header->entry_size;
    if (*entries_size > GPT_MAX_ENTRY_BYTES) return NULL;
    uint64_t entry_sectors = (*entries_size + SATA_SECTOR_SIZE - 1) / SATA_SECTOR_SIZE;
    if (header->entry_lba >= disk_sectors || entry_sectors > disk_sectors - header->entry_lba) return NULL;
    uint8_t *entries = malloc(*entries_size);
    if (!entries) return NULL;
    uint64_t result = read_sata_device(entries, *entries_size, header->entry_lba * SATA_SECTOR_SIZE, disk_index);
    if (result == *entries_size && gpt_crc32(entries, *entries_size) == header->entry_crc32) return entries;
    free(entries);
    return NULL;
}

static bool gpt_register_partition(int disk_index, const char *disk_name, int number, uint64_t first_lba, uint64_t last_lba, uint64_t disk_sectors) {
    if (gpt_partition_count >= GPT_MAX_PARTITIONS || first_lba > last_lba || first_lba >= disk_sectors || last_lba >= disk_sectors) return false;
    uint64_t sectors = last_lba - first_lba + 1;
    if (sectors > UINT64_MAX / SATA_SECTOR_SIZE) return false;
    char name[24];
    if (!gpt_make_partition_name(name, sizeof(name), disk_name, number)) return false;
    int index = gpt_partition_count;
    gpt_partitions[index].disk_index = disk_index;
    gpt_partitions[index].offset = first_lba * SATA_SECTOR_SIZE;
    gpt_partitions[index].size = sectors * SATA_SECTOR_SIZE;
    if (register_block_device_idx(name, read_gpt_partition, write_gpt_partition, index, gpt_partitions[index].size) < 0) return false;
    gpt_partition_count++;
    log("gpt: registered %s, lba %llu-%llu\n", name, first_lba, last_lba);
    return true;
}

bool gpt_probe_sata_disk(int disk_index, const char *disk_name, uint64_t disk_size) {
    if (!disk_name || disk_size < SATA_SECTOR_SIZE * 2) return false;
    uint64_t disk_sectors = disk_size / SATA_SECTOR_SIZE;
    gpt_header_t header;
    bool using_backup = false;
    if (!gpt_read_header(disk_index, 1, disk_sectors, &header)) {
        if (!gpt_read_header(disk_index, disk_sectors - 1, disk_sectors, &header)) return false;
        using_backup = true;
    }
    uint64_t entries_size;
    uint8_t *entries = gpt_read_entries(disk_index, disk_sectors, &header, &entries_size);
    if (!entries && !using_backup) {
        if (!gpt_read_header(disk_index, disk_sectors - 1, disk_sectors, &header)) return false;
        entries = gpt_read_entries(disk_index, disk_sectors, &header, &entries_size);
        using_backup = true;
    }
    if (!entries) return false;
    if (using_backup) log("gpt: using backup table on %s\n", disk_name);

    bool found = false;
    for (uint32_t index = 0; index < header.entry_count; index++) {
        gpt_entry_t *entry = (gpt_entry_t *)(entries + (uint64_t)index * header.entry_size);
        if (gpt_guid_empty(entry->type_guid)) continue;
        if (entry->first_lba < header.first_usable_lba || entry->last_lba > header.last_usable_lba) continue;
        if (gpt_register_partition(disk_index, disk_name, index + 1, entry->first_lba, entry->last_lba, disk_sectors)) found = true;
    }
    free(entries);
    return found;
}
