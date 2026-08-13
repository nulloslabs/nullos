#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <io/ahci.h>
#include <io/sata.h>

static uint64_t sata_sizes[SATA_MAX_DEVICES];
static int sata_devices_found;
bool is_sata_present;

int sata_device_count(void) { return sata_devices_found; }

bool sata_device_size(int index, uint64_t *size) {
    if (index < 0 || index >= sata_devices_found || !size || !sata_sizes[index]) return false;
    *size = sata_sizes[index];
    return true;
}

bool make_sata_disk_name(char *name, uint64_t name_size, int index) {
    if (!name || name_size < 4 || index < 0 || index >= 26) return false;
    name[0] = 's';
    name[1] = 'd';
    name[2] = 'a' + index;
    name[3] = '\0';
    return true;
}

uint64_t read_sata_device(void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= sata_devices_found || !sata_sizes[index]) return (uint64_t)-ENODEV;
    if (!count) return 0;
    if (!data) return (uint64_t)-EINVAL;
    uint64_t size = sata_sizes[index];
    if (offset >= size) return 0;
    if (count > size - offset) count = size - offset;

    uint8_t sector[SATA_SECTOR_SIZE];
    uint64_t completed = 0;
    while (completed < count) {
        uint64_t position = offset + completed;
        uint64_t lba = position / SATA_SECTOR_SIZE;
        uint32_t sector_offset = position % SATA_SECTOR_SIZE;
        uint64_t remaining = count - completed;
        if (!sector_offset && remaining >= SATA_SECTOR_SIZE) {
            uint32_t sectors = remaining / SATA_SECTOR_SIZE;
            uint32_t maximum = AHCI_MAX_TRANSFER_SIZE / SATA_SECTOR_SIZE;
            if (sectors > maximum) sectors = maximum;
            int status = ahci_read_sectors(index, lba, sectors, (uint8_t *)data + completed);
            if (status < 0) return (uint64_t)status;
            completed += (uint64_t)sectors * SATA_SECTOR_SIZE;
            continue;
        }
        int status = ahci_read_sectors(index, lba, 1, sector);
        if (status < 0) return (uint64_t)status;
        uint64_t bytes = SATA_SECTOR_SIZE - sector_offset;
        if (bytes > remaining) bytes = remaining;
        memcpy((uint8_t *)data + completed, sector + sector_offset, bytes);
        completed += bytes;
    }
    return count;
}

uint64_t write_sata_device(const void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= sata_devices_found || !sata_sizes[index]) return (uint64_t)-ENODEV;
    if (!count) return 0;
    if (!data) return (uint64_t)-EINVAL;
    uint64_t size = sata_sizes[index];
    if (offset >= size) return (uint64_t)-ENOSPC;
    if (count > size - offset) count = size - offset;

    uint8_t sector[SATA_SECTOR_SIZE];
    uint64_t completed = 0;
    while (completed < count) {
        uint64_t position = offset + completed;
        uint64_t lba = position / SATA_SECTOR_SIZE;
        uint32_t sector_offset = position % SATA_SECTOR_SIZE;
        uint64_t remaining = count - completed;
        if (!sector_offset && remaining >= SATA_SECTOR_SIZE) {
            uint32_t sectors = remaining / SATA_SECTOR_SIZE;
            uint32_t maximum = AHCI_MAX_TRANSFER_SIZE / SATA_SECTOR_SIZE;
            if (sectors > maximum) sectors = maximum;
            int status = ahci_write_sectors(index, lba, sectors, (const uint8_t *)data + completed);
            if (status < 0) return (uint64_t)status;
            completed += (uint64_t)sectors * SATA_SECTOR_SIZE;
            continue;
        }
        int status = ahci_read_sectors(index, lba, 1, sector);
        if (status < 0) return (uint64_t)status;
        uint64_t bytes = SATA_SECTOR_SIZE - sector_offset;
        if (bytes > remaining) bytes = remaining;
        memcpy(sector + sector_offset, (const uint8_t *)data + completed, bytes);
        status = ahci_write_sectors(index, lba, 1, sector);
        if (status < 0) return (uint64_t)status;
        completed += bytes;
    }
    return count;
}

void init_sata(void) {
    sata_devices_found = ahci_device_count();
    if (sata_devices_found > SATA_MAX_DEVICES) sata_devices_found = SATA_MAX_DEVICES;
    for (int index = 0; index < sata_devices_found; index++) {
        ahci_device_info_t info;
        if (!ahci_device_info(index, &info) || info.sector_size != SATA_SECTOR_SIZE || info.sectors > UINT64_MAX / SATA_SECTOR_SIZE) {
            sata_sizes[index] = 0;
            continue;
        }
        sata_sizes[index] = info.sectors * SATA_SECTOR_SIZE;
    }
    is_sata_present = sata_devices_found > 0;
    if (is_sata_present) log("sata: initialized sata with %d device(s)\n", sata_devices_found);
    else log("sata: no sata drive found\n");
}

