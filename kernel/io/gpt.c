#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <autoconf.h>
#include <main/string.h>
#include <mm/mm.h>
#include <io/devices.h>
#include <io/gpt.h>
#include <io/sata.h>
#include <io/pata.h>

static gpt_partition_t gpt_partitions[GPT_MAX_PARTITIONS];
static int gpt_partition_count;

static uint32_t compute_gpt_crc32(const void *data, uint64_t size) {
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (uint64_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
    return ~crc;
}

static bool is_gpt_guid_empty(const uint8_t guid[16]) {
    for (int i = 0; i < 16; i++) if (guid[i]) return false;
    return true;
}

static bool make_gpt_partition_name(char *name, uint64_t name_size, const char *disk_name, int number) {
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
    if (index < 0 || index >= gpt_partition_count || !gpt_partitions[index].active) return (uint64_t)-ENODEV;
    gpt_partition_t *partition = &gpt_partitions[index];
    if (offset >= partition->size) return 0;
    if (count > partition->size - offset) count = partition->size - offset;
    if (partition->is_pata) {
#ifdef CONFIG_PATA
        return read_pata_device(data, count, partition->offset + offset, partition->disk_index);
#else
        return (uint64_t)-ENODEV;
#endif
    } else {
#ifdef CONFIG_SATA
        return read_sata_device(data, count, partition->offset + offset, partition->disk_index);
#else
        return (uint64_t)-ENODEV;
#endif
    }
}

static uint64_t write_gpt_partition(const void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= gpt_partition_count || !gpt_partitions[index].active) return (uint64_t)-ENODEV;
    gpt_partition_t *partition = &gpt_partitions[index];
    if (offset >= partition->size) return (uint64_t)-ENOSPC;
    if (count > partition->size - offset) count = partition->size - offset;
    if (partition->is_pata) {
#ifdef CONFIG_PATA
        return write_pata_device(data, count, partition->offset + offset, partition->disk_index);
#else
        return (uint64_t)-ENOSPC;
#endif
    } else {
#ifdef CONFIG_SATA
        return write_sata_device(data, count, partition->offset + offset, partition->disk_index);
#else
        return (uint64_t)-ENOSPC;
#endif
    }
}

static bool read_gpt_header(int disk_index, uint64_t header_lba, uint64_t disk_sectors, gpt_header_t *header, bool is_pata) {
    uint64_t sector_size = is_pata ? PATA_SECTOR_SIZE : SATA_SECTOR_SIZE;
    uint8_t sector[SATA_SECTOR_SIZE];
    if (header_lba >= disk_sectors) return false;
#if defined(CONFIG_PATA) && defined(CONFIG_SATA)
    uint64_t result = is_pata ? read_pata_device(sector, sizeof(sector), header_lba * sector_size, disk_index)
                              : read_sata_device(sector, sizeof(sector), header_lba * sector_size, disk_index);
#elif defined(CONFIG_PATA)
    uint64_t result = read_pata_device(sector, sizeof(sector), header_lba * sector_size, disk_index);
#elif defined(CONFIG_SATA)
    uint64_t result = read_sata_device(sector, sizeof(sector), header_lba * sector_size, disk_index);
#else
    uint64_t result = 0;
#endif
    if (result != sizeof(sector)) return false;
    memcpy(header, sector, sizeof(*header));
    if (header->signature != GPT_SIGNATURE || header->header_size < GPT_MIN_HEADER_SIZE || header->header_size > SATA_SECTOR_SIZE) return false;
    if (header->current_lba != header_lba || header->backup_lba >= disk_sectors || header->first_usable_lba > header->last_usable_lba || header->last_usable_lba >= disk_sectors) return false;
    uint32_t expected_crc = header->header_crc32;
    ((gpt_header_t *)sector)->header_crc32 = 0;
    if (compute_gpt_crc32(sector, header->header_size) != expected_crc) return false;
    return true;
}

static uint8_t *read_gpt_entries(int disk_index, uint64_t disk_sectors, const gpt_header_t *header, uint64_t *entries_size, bool is_pata) {
    uint64_t sector_size = is_pata ? PATA_SECTOR_SIZE : SATA_SECTOR_SIZE;
    if (header->entry_size < GPT_MIN_ENTRY_SIZE || header->entry_size % 8 || !header->entry_count) return NULL;
    if (header->entry_count > UINT64_MAX / header->entry_size) return NULL;
    *entries_size = (uint64_t)header->entry_count * header->entry_size;
    if (*entries_size > GPT_MAX_ENTRY_BYTES) return NULL;
    uint64_t entry_sectors = (*entries_size + sector_size - 1) / sector_size;
    if (header->entry_lba >= disk_sectors || entry_sectors > disk_sectors - header->entry_lba) return NULL;
    uint8_t *entries = malloc(*entries_size);
    if (!entries) return NULL;
#if defined(CONFIG_PATA) && defined(CONFIG_SATA)
    uint64_t result = is_pata ? read_pata_device(entries, *entries_size, header->entry_lba * sector_size, disk_index)
                              : read_sata_device(entries, *entries_size, header->entry_lba * sector_size, disk_index);
#elif defined(CONFIG_PATA)
    uint64_t result = read_pata_device(entries, *entries_size, header->entry_lba * sector_size, disk_index);
#elif defined(CONFIG_SATA)
    uint64_t result = read_sata_device(entries, *entries_size, header->entry_lba * sector_size, disk_index);
#else
    uint64_t result = 0;
#endif
    if (result == *entries_size && compute_gpt_crc32(entries, *entries_size) == header->entry_crc32) return entries;
    free(entries);
    return NULL;
}

static bool register_gpt_partition(int disk_index, const char *disk_name, int number, uint64_t first_lba, uint64_t last_lba, uint64_t disk_sectors, bool is_pata) {
    if (first_lba > last_lba || first_lba >= disk_sectors || last_lba >= disk_sectors) return false;
    uint64_t sectors = last_lba - first_lba + 1;
    uint64_t sector_size = is_pata ? PATA_SECTOR_SIZE : SATA_SECTOR_SIZE;
    if (sectors > UINT64_MAX / sector_size) return false;
    char name[24];
    if (!make_gpt_partition_name(name, sizeof(name), disk_name, number)) return false;
    int index = -1;
    for (int i = 0; i < gpt_partition_count; i++) {
        if (!gpt_partitions[i].active) { index = i; break; }
    }
    if (index < 0) {
        if (gpt_partition_count >= GPT_MAX_PARTITIONS) return false;
        index = gpt_partition_count++;
    }
    gpt_partitions[index].disk_index = disk_index;
    gpt_partitions[index].offset = first_lba * sector_size;
    gpt_partitions[index].size = sectors * sector_size;
    gpt_partitions[index].is_pata = is_pata;
    if (register_block_device_idx(name, read_gpt_partition, write_gpt_partition, index, gpt_partitions[index].size) < 0) return false;
    strcpy(gpt_partitions[index].name, name);
    gpt_partitions[index].active = true;
    return true;
}

#if defined(CONFIG_SATA) && defined(CONFIG_GPT)
bool probe_gpt_for_sata_disk(int disk_index, const char *disk_name, uint64_t disk_size) {
    if (!disk_name || disk_size < SATA_SECTOR_SIZE * 2) return false;
    uint64_t disk_sectors = disk_size / SATA_SECTOR_SIZE;
    gpt_header_t header;
    bool using_backup = false;
    if (!read_gpt_header(disk_index, 1, disk_sectors, &header, false)) {
        if (!read_gpt_header(disk_index, disk_sectors - 1, disk_sectors, &header, false)) return false;
        using_backup = true;
    }
    uint64_t entries_size;
    uint8_t *entries = read_gpt_entries(disk_index, disk_sectors, &header, &entries_size, false);
    if (!entries && !using_backup) {
        if (!read_gpt_header(disk_index, disk_sectors - 1, disk_sectors, &header, false)) return false;
        entries = read_gpt_entries(disk_index, disk_sectors, &header, &entries_size, false);
        using_backup = true;
    }
    if (!entries) return false;
    bool found = false;
    for (uint32_t index = 0; index < header.entry_count; index++) {
        gpt_entry_t *entry = (gpt_entry_t *)(entries + (uint64_t)index * header.entry_size);
        if (is_gpt_guid_empty(entry->type_guid)) continue;
        if (entry->first_lba < header.first_usable_lba || entry->last_lba > header.last_usable_lba) continue;
        if (register_gpt_partition(disk_index, disk_name, index + 1, entry->first_lba, entry->last_lba, disk_sectors, false)) found = true;
    }
    free(entries);
    return found;
}
#endif

#if defined(CONFIG_PATA) && defined(CONFIG_GPT)
bool probe_gpt_for_pata_disk(int disk_index, const char *disk_name, uint64_t disk_size) {
    if (!disk_name || disk_size < PATA_SECTOR_SIZE * 2) return false;
    uint64_t disk_sectors = disk_size / PATA_SECTOR_SIZE;
    gpt_header_t header;
    bool using_backup = false;
    if (!read_gpt_header(disk_index, 1, disk_sectors, &header, true)) {
        if (!read_gpt_header(disk_index, disk_sectors - 1, disk_sectors, &header, true)) return false;
        using_backup = true;
    }
    uint64_t entries_size;
    uint8_t *entries = read_gpt_entries(disk_index, disk_sectors, &header, &entries_size, true);
    if (!entries && !using_backup) {
        if (!read_gpt_header(disk_index, disk_sectors - 1, disk_sectors, &header, true)) return false;
        entries = read_gpt_entries(disk_index, disk_sectors, &header, &entries_size, true);
        using_backup = true;
    }
    if (!entries) return false;
    bool found = false;
    for (uint32_t index = 0; index < header.entry_count; index++) {
        gpt_entry_t *entry = (gpt_entry_t *)(entries + (uint64_t)index * header.entry_size);
        if (is_gpt_guid_empty(entry->type_guid)) continue;
        if (entry->first_lba < header.first_usable_lba || entry->last_lba > header.last_usable_lba) continue;
        if (register_gpt_partition(disk_index, disk_name, index + 1, entry->first_lba, entry->last_lba, disk_sectors, true)) found = true;
    }
    free(entries);
    return found;
}
#endif

void remove_gpt_partitions(int disk_index, bool is_pata) {
    for (int i = 0; i < gpt_partition_count; i++) {
        if (!gpt_partitions[i].active || gpt_partitions[i].disk_index != disk_index || gpt_partitions[i].is_pata != is_pata) continue;
        unregister_device(gpt_partitions[i].name);
        gpt_partitions[i].active = false;
        gpt_partitions[i].name[0] = '\0';
    }
}
