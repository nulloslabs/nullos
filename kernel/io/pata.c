#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <io/ide.h>
#include <io/io.h>
#include <io/pata.h>

static ide_device_t pata_devices[IDE_MAX_DEVICES];
static uint64_t pata_sectors[IDE_MAX_DEVICES];
static bool pata_ready[IDE_MAX_DEVICES];
static int first_pata = -1;

static int identify_pata(const ide_device_t *device, uint64_t *sectors) {
    select_ide_device(device);
    outb(device->io_base + IDE_REG_SECTOR_COUNT, 0);
    outb(device->io_base + IDE_REG_LBA_LOW, 0);
    outb(device->io_base + IDE_REG_LBA_MID, 0);
    outb(device->io_base + IDE_REG_LBA_HIGH, 0);
    outb(device->io_base + IDE_REG_COMMAND, PATA_COMMAND_IDENTIFY);
    uint8_t status = inb(device->io_base + IDE_REG_STATUS);
    if (!status || status == 0xFF || wait_ide_not_busy(device) < 0) return -ENODEV;
    if (inb(device->io_base + IDE_REG_LBA_MID) || inb(device->io_base + IDE_REG_LBA_HIGH)) return -ENODEV;
    if (wait_ide_drq(device) < 0) return -ENODEV;

    uint16_t identify[256];
    for (int i = 0; i < 256; i++) identify[i] = inw(device->io_base + IDE_REG_DATA);
    if (!(identify[49] & (1 << 9))) return -EOPNOTSUPP;
    *sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    return *sectors ? 0 : -ENODEV;
}

static int transfer_pata(int index, uint32_t lba, uint8_t sectors, bool write) {
    ide_device_t *device = &pata_devices[index];
    uint32_t count = sectors ? sectors : 256;
    int status = prepare_ide_dma(device, count * PATA_SECTOR_SIZE, !write);
    if (status < 0) return status;
    if (wait_ide_not_busy(device) < 0) return -EIO;

    outb(device->io_base + IDE_REG_DRIVE, 0xE0 | (device->slave << 4) | ((lba >> 24) & 0x0F));
    delay_ide_400ns(device);
    outb(device->io_base + IDE_REG_SECTOR_COUNT, sectors);
    outb(device->io_base + IDE_REG_LBA_LOW, lba & 0xFF);
    outb(device->io_base + IDE_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(device->io_base + IDE_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(device->io_base + IDE_REG_COMMAND, write ? PATA_COMMAND_WRITE_DMA : PATA_COMMAND_READ_DMA);
    return start_ide_dma(device);
}

static int flush_pata_cache(int index) {
    ide_device_t *device = &pata_devices[index];
    outb(device->io_base + IDE_REG_DRIVE, 0xE0 | (device->slave << 4));
    outb(device->io_base + IDE_REG_COMMAND, PATA_COMMAND_CACHE_FLUSH);
    return wait_ide_not_busy(device);
}

static int read_pata_index(int index, void *data, uint64_t count, uint64_t offset) {
    if (index < 0 || index >= IDE_MAX_DEVICES || !pata_ready[index]) return -ENODEV;
    if (!count) return 0;
    if (!data) return -EINVAL;
    uint64_t sectors = pata_sectors[index] < PATA_LBA28_LIMIT ? pata_sectors[index] : PATA_LBA28_LIMIT;
    uint64_t size = sectors * PATA_SECTOR_SIZE;
    if (offset >= size || count > size - offset) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&ide_lock, &flags);
    uint64_t completed = 0;
    int status = 0;
    while (completed < count) {
        uint64_t position = offset + completed;
        uint32_t lba = position / PATA_SECTOR_SIZE;
        uint32_t sector_offset = position % PATA_SECTOR_SIZE;
        uint64_t remaining = count - completed;
        uint32_t chunk = 1;
        if (!sector_offset && remaining >= PATA_SECTOR_SIZE) {
            chunk = remaining / PATA_SECTOR_SIZE;
            if (chunk > PATA_DMA_MAX_SECTORS) chunk = PATA_DMA_MAX_SECTORS;
        }
        status = transfer_pata(index, lba, (uint8_t)chunk, false);
        if (status < 0) break;
        uint64_t bytes = (uint64_t)chunk * PATA_SECTOR_SIZE - sector_offset;
        if (bytes > remaining) bytes = remaining;
        memcpy((uint8_t *)data + completed, ide_dma_data + sector_offset, bytes);
        completed += bytes;
    }
    spin_unlock_irqrestore(&ide_lock, flags);
    return status;
}

static int write_pata_index(int index, const void *data, uint64_t count, uint64_t offset) {
    if (index < 0 || index >= IDE_MAX_DEVICES || !pata_ready[index]) return -ENODEV;
    if (!count) return 0;
    if (!data) return -EINVAL;
    uint64_t sectors = pata_sectors[index] < PATA_LBA28_LIMIT ? pata_sectors[index] : PATA_LBA28_LIMIT;
    uint64_t size = sectors * PATA_SECTOR_SIZE;
    if (offset >= size || count > size - offset) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&ide_lock, &flags);
    uint64_t completed = 0;
    int status = 0;
    while (completed < count) {
        uint64_t position = offset + completed;
        uint32_t lba = position / PATA_SECTOR_SIZE;
        uint32_t sector_offset = position % PATA_SECTOR_SIZE;
        uint64_t remaining = count - completed;
        uint32_t chunk = 1;
        uint64_t bytes;
        if (!sector_offset && remaining >= PATA_SECTOR_SIZE) {
            chunk = remaining / PATA_SECTOR_SIZE;
            if (chunk > PATA_DMA_MAX_SECTORS) chunk = PATA_DMA_MAX_SECTORS;
            bytes = (uint64_t)chunk * PATA_SECTOR_SIZE;
        } else {
            status = transfer_pata(index, lba, 1, false);
            if (status < 0) break;
            bytes = PATA_SECTOR_SIZE - sector_offset;
            if (bytes > remaining) bytes = remaining;
        }
        memcpy(ide_dma_data + sector_offset, (const uint8_t *)data + completed, bytes);
        status = transfer_pata(index, lba, (uint8_t)chunk, true);
        if (status < 0) break;
        completed += bytes;
    }
    if (status == 0) status = flush_pata_cache(index);
    spin_unlock_irqrestore(&ide_lock, flags);
    return status;
}

int read_pata(void *data, uint64_t count, uint64_t offset) { return read_pata_index(first_pata, data, count, offset); }

int write_pata(const void *data, uint64_t count, uint64_t offset) { return write_pata_index(first_pata, data, count, offset); }

uint64_t read_pata_device(void *data, uint64_t count, uint64_t offset, int index) {
    uint64_t sectors = pata_sectors[index] < PATA_LBA28_LIMIT ? pata_sectors[index] : PATA_LBA28_LIMIT;
    uint64_t size = sectors * PATA_SECTOR_SIZE;
    if (offset >= size) return 0;
    if (count > size - offset) count = size - offset;
    int status = read_pata_index(index, data, count, offset);
    return status < 0 ? (uint64_t)status : count;
}

uint64_t write_pata_device(const void *data, uint64_t count, uint64_t offset, int index) {
    uint64_t sectors = pata_sectors[index] < PATA_LBA28_LIMIT ? pata_sectors[index] : PATA_LBA28_LIMIT;
    uint64_t size = sectors * PATA_SECTOR_SIZE;
    if (offset >= size) return (uint64_t)-ENOSPC;
    if (count > size - offset) count = size - offset;
    int status = write_pata_index(index, data, count, offset);
    return status < 0 ? (uint64_t)status : count;
}

bool pata_device_size(int index, uint64_t *size) {
    if (index < 0 || index >= IDE_MAX_DEVICES || !pata_ready[index] || !size) return false;
    uint64_t sectors = pata_sectors[index] < PATA_LBA28_LIMIT ? pata_sectors[index] : PATA_LBA28_LIMIT;
    *size = sectors * PATA_SECTOR_SIZE;
    return true;
}

void init_pata(void) {
    int found = 0;
    for (uint8_t i = 0; i < IDE_MAX_DEVICES; i++) {
        ide_device_t device;
        uint64_t sectors;
        get_ide_device(i, &device);
        if (identify_pata(&device, &sectors) < 0) continue;
        pata_devices[i] = device;
        pata_sectors[i] = sectors;
        pata_ready[i] = true;
        if (first_pata < 0) first_pata = i;
        found++;
    }

    if (!found) {
        log("pata: no pata drive found\n");
        return;
    }
    log("pata: initialized pata\n");
}
