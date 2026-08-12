#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/atapi.h>
#include <io/ide.h>
#include <io/io.h>

static ide_device_t atapi_devices[IDE_MAX_DEVICES];
static uint64_t atapi_sectors[IDE_MAX_DEVICES];
static bool atapi_ready[IDE_MAX_DEVICES];
static int first_atapi = -1;

static uint32_t read_atapi_be32(const uint8_t *data) { return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3]; }

static void write_atapi_packet(const ide_device_t *device, const uint8_t packet[12]) {
    for (int i = 0; i < 6; i++) outw(device->io_base + IDE_REG_DATA, (uint16_t)packet[i * 2] | ((uint16_t)packet[i * 2 + 1] << 8));
}

static int identify_atapi(const ide_device_t *device) {
    select_ide_device(device);
    outb(device->io_base + IDE_REG_SECTOR_COUNT, 0);
    outb(device->io_base + IDE_REG_LBA_LOW, 0);
    outb(device->io_base + IDE_REG_LBA_MID, 0);
    outb(device->io_base + IDE_REG_LBA_HIGH, 0);
    outb(device->io_base + IDE_REG_COMMAND, ATAPI_COMMAND_IDENTIFY_PACKET);
    uint8_t status = inb(device->io_base + IDE_REG_STATUS);
    if (!status || status == 0xFF || wait_ide_not_busy(device) < 0 || wait_ide_drq(device) < 0) return -ENODEV;

    uint16_t identify[256];
    for (int i = 0; i < 256; i++) identify[i] = inw(device->io_base + IDE_REG_DATA);
    return identify[49] & (1 << 8) ? 0 : -EOPNOTSUPP;
}

static int send_atapi_packet_pio(const ide_device_t *device, const uint8_t packet[12], void *data, uint16_t size) {
    select_ide_device(device);
    outb(device->io_base + IDE_REG_FEATURES, 0);
    outb(device->io_base + IDE_REG_LBA_MID, size & 0xFF);
    outb(device->io_base + IDE_REG_LBA_HIGH, size >> 8);
    outb(device->io_base + IDE_REG_COMMAND, ATAPI_COMMAND_PACKET);
    if (wait_ide_drq(device) < 0) return -EIO;
    write_atapi_packet(device, packet);
    if (wait_ide_drq(device) < 0) return -EIO;

    uint16_t available = inb(device->io_base + IDE_REG_LBA_MID) | ((uint16_t)inb(device->io_base + IDE_REG_LBA_HIGH) << 8);
    uint8_t *output = data;
    uint32_t word_count = ((uint32_t)available + 1U) / 2U;
    for (uint32_t i = 0; i < word_count; i++) {
        uint16_t word = inw(device->io_base + IDE_REG_DATA);
        uint32_t offset = i * 2;
        if (offset < size) output[offset] = word & 0xFF;
        if (offset + 1 < size) output[offset + 1] = word >> 8;
    }
    return wait_ide_not_busy(device);
}

static int get_atapi_capacity(const ide_device_t *device, uint64_t *sectors) {
    uint8_t packet[12] = {0};
    uint8_t response[8];
    packet[0] = ATAPI_PACKET_READ_CAPACITY;
    int status = send_atapi_packet_pio(device, packet, response, sizeof(response));
    if (status < 0) return status;
    uint32_t last_lba = read_atapi_be32(response);
    uint32_t sector_size = read_atapi_be32(response + 4);
    if (sector_size != ATAPI_SECTOR_SIZE) return -EOPNOTSUPP;
    *sectors = (uint64_t)last_lba + 1;
    return 0;
}

static int transfer_atapi(int index, uint32_t lba, uint32_t sectors) {
    ide_device_t *device = &atapi_devices[index];
    uint32_t bytes = sectors * ATAPI_SECTOR_SIZE;
    int status = prepare_ide_dma(device, bytes, true);
    if (status < 0) return status;

    uint8_t packet[12] = {0};
    packet[0] = ATAPI_PACKET_READ_12;
    packet[2] = lba >> 24;
    packet[3] = lba >> 16;
    packet[4] = lba >> 8;
    packet[5] = lba;
    packet[6] = sectors >> 24;
    packet[7] = sectors >> 16;
    packet[8] = sectors >> 8;
    packet[9] = sectors;

    select_ide_device(device);
    outb(device->io_base + IDE_REG_FEATURES, 1);
    outb(device->io_base + IDE_REG_LBA_MID, bytes & 0xFF);
    outb(device->io_base + IDE_REG_LBA_HIGH, (bytes >> 8) & 0xFF);
    outb(device->io_base + IDE_REG_COMMAND, ATAPI_COMMAND_PACKET);
    if (wait_ide_drq(device) < 0) return -EIO;
    write_atapi_packet(device, packet);
    return start_ide_dma(device);
}

static int read_atapi_index(int index, void *data, uint64_t count, uint64_t offset) {
    if (index < 0 || index >= IDE_MAX_DEVICES || !atapi_ready[index]) return -ENODEV;
    if (!count) return 0;
    if (!data) return -EINVAL;
    uint64_t size = atapi_sectors[index] * ATAPI_SECTOR_SIZE;
    if (offset >= size || count > size - offset) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&ide_lock, &flags);
    uint64_t completed = 0;
    int status = 0;
    while (completed < count) {
        uint64_t position = offset + completed;
        uint32_t lba = position / ATAPI_SECTOR_SIZE;
        uint32_t sector_offset = position % ATAPI_SECTOR_SIZE;
        uint64_t remaining = count - completed;
        uint32_t chunk = 1;
        if (!sector_offset && remaining >= ATAPI_SECTOR_SIZE) {
            chunk = remaining / ATAPI_SECTOR_SIZE;
            if (chunk > ATAPI_DMA_MAX_SECTORS) chunk = ATAPI_DMA_MAX_SECTORS;
        }
        status = transfer_atapi(index, lba, chunk);
        if (status < 0) break;
        uint64_t bytes = (uint64_t)chunk * ATAPI_SECTOR_SIZE - sector_offset;
        if (bytes > remaining) bytes = remaining;
        memcpy((uint8_t *)data + completed, ide_dma_data + sector_offset, bytes);
        completed += bytes;
    }
    spin_unlock_irqrestore(&ide_lock, flags);
    return status;
}

int read_atapi(void *data, uint64_t count, uint64_t offset) { return read_atapi_index(first_atapi, data, count, offset); }

uint64_t read_atapi_device(void *data, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= IDE_MAX_DEVICES || !atapi_ready[index]) return (uint64_t)-ENODEV;
    uint64_t size = atapi_sectors[index] * ATAPI_SECTOR_SIZE;
    if (offset >= size) return 0;
    if (count > size - offset) count = size - offset;
    int status = read_atapi_index(index, data, count, offset);
    return status < 0 ? (uint64_t)status : count;
}

uint64_t write_atapi_device(const void *data, uint64_t count, uint64_t offset, int index) {
    (void)data;
    (void)count;
    (void)offset;
    (void)index;
    return (uint64_t)-EROFS;
}

bool atapi_device_size(int index, uint64_t *size) {
    if (index < 0 || index >= IDE_MAX_DEVICES || !atapi_ready[index] || !size) return false;
    *size = atapi_sectors[index] * ATAPI_SECTOR_SIZE;
    return true;
}

void init_atapi(void) {
    int found = 0;
    uint64_t flags;
    spin_lock_irqsave(&ide_lock, &flags);
    for (uint8_t i = 0; i < IDE_MAX_DEVICES; i++) {
        ide_device_t device;
        uint64_t sectors;
        get_ide_device(i, &device);
        if (identify_atapi(&device) < 0 || get_atapi_capacity(&device, &sectors) < 0) continue;
        int index = found;
        atapi_devices[index] = device;
        atapi_sectors[index] = sectors;
        atapi_ready[index] = true;
        if (first_atapi < 0) first_atapi = index;
        found++;
    }
    spin_unlock_irqrestore(&ide_lock, flags);
    if (found) log("atapi: initialized atapi\n");
    else log("atapi: no atapi drive found\n");
}
