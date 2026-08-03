#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <io/pci.h>

#define IDE_CLASS        0x01
#define IDE_SUBCLASS     0x01
#define IDE_PROGIF_MASK  0x80
#define IDE_PROGIF_VALUE 0x80

#define IDE_PRIMARY_IO        0x1F0
#define IDE_PRIMARY_CONTROL   0x3F6
#define IDE_SECONDARY_IO      0x170
#define IDE_SECONDARY_CONTROL 0x376

#define IDE_REG_DATA         0
#define IDE_REG_FEATURES     1
#define IDE_REG_ERROR        1
#define IDE_REG_SECTOR_COUNT 2
#define IDE_REG_LBA_LOW      3
#define IDE_REG_LBA_MID      4
#define IDE_REG_LBA_HIGH     5
#define IDE_REG_DRIVE        6
#define IDE_REG_STATUS       7
#define IDE_REG_COMMAND      7

#define IDE_STATUS_ERROR 0x01
#define IDE_STATUS_DRQ   0x08
#define IDE_STATUS_FAULT 0x20
#define IDE_STATUS_READY 0x40
#define IDE_STATUS_BUSY  0x80

#define IDE_BM_COMMAND 0
#define IDE_BM_STATUS  2
#define IDE_BM_PRDT    4

#define IDE_BM_START     0x01
#define IDE_BM_READ      0x08
#define IDE_BM_ACTIVE    0x01
#define IDE_BM_ERROR     0x02
#define IDE_BM_INTERRUPT 0x04

#define IDE_DMA_BUFFER_SIZE 65536
#define IDE_TIMEOUT         1000000
#define IDE_MAX_DEVICES     4

typedef struct {
    uint16_t io_base;
    uint16_t control_base;
    uint16_t bus_master_base;
    uint8_t slave;
} ide_device_t;

typedef struct {
    uint32_t address;
    uint16_t byte_count;
    uint16_t flags;
} __attribute__((packed)) ide_prd_t;

void get_ide_device(uint8_t index, ide_device_t *device);
void select_ide_device(const ide_device_t *device);
void delay_ide_400ns(const ide_device_t *device);
int wait_ide_not_busy(const ide_device_t *device);
int wait_ide_drq(const ide_device_t *device);
void lock_ide(uint64_t *flags);
void unlock_ide(uint64_t flags);
uint8_t *get_ide_dma_data(void);
int prepare_ide_dma(const ide_device_t *device, uint32_t bytes, bool read);
int start_ide_dma(const ide_device_t *device);
bool ide_has_pata(void);
bool ide_has_atapi(void);
bool make_ide_disk_name(char *name, size_t name_size, const char *prefix, uint64_t index);
bool make_ide_numbered_name(char *name, size_t name_size, const char *prefix, uint64_t index);
bool init_ide(pci_device_t *dev);
