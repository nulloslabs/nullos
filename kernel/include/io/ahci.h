#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <io/pci.h>

#define AHCI_CLASS             0x01
#define AHCI_SUBCLASS          0x06
#define AHCI_PROGIF            0x01
#define AHCI_SECTOR_SIZE       512
#define AHCI_MAX_DEVICES       8
#define AHCI_MAX_TRANSFER_SIZE (128 * 1024)

typedef struct {
    uint64_t sectors;
    uint32_t sector_size;
    char model[41];
} ahci_device_info_t;

int ahci_device_count(void);
bool ahci_device_info(int index, ahci_device_info_t *info);
int ahci_read_sectors(int index, uint64_t lba, uint32_t sectors, void *buffer);
int ahci_write_sectors(int index, uint64_t lba, uint32_t sectors, const void *buffer);
bool init_ahci(pci_device_t *dev);
