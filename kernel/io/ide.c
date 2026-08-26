#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/spinlocks.h>
#include <io/ide.h>
#include <io/io.h>
#include <mm/vmm.h>

static uint16_t ide_channels[2][3];
static uint8_t ide_bus = 0;
static uint8_t ide_dev = 0;
static uint8_t ide_func = 0;
static bool ide_ready = false;
static ide_prd_t ide_prdt[2] __attribute__((aligned(16)));

bool is_pata_present = false;
bool is_atapi_present = false;
spinlock_t ide_lock = SPINLOCK_INIT;
uint8_t ide_dma_data[IDE_DMA_BUFFER_SIZE] __attribute__((aligned(PAGE_SIZE)));

void delay_ide_400ns(const ide_device_t *device) {
    inb(device->control_base);
    inb(device->control_base);
    inb(device->control_base);
    inb(device->control_base);
}

void select_ide_device(const ide_device_t *device) {
    outb(device->io_base + IDE_REG_DRIVE, 0xA0 | (device->slave << 4));
    delay_ide_400ns(device);
}

int wait_ide_not_busy(const ide_device_t *device) {
    for (uint32_t i = 0; i < IDE_TIMEOUT; i++) {
        uint8_t status = inb(device->io_base + IDE_REG_STATUS);
        if (!(status & IDE_STATUS_BUSY)) return status & (IDE_STATUS_ERROR | IDE_STATUS_FAULT) ? -EIO : 0;
        __asm__ volatile ("pause");
    }
    return -ETIMEDOUT;
}

int wait_ide_drq(const ide_device_t *device) {
    for (uint32_t i = 0; i < IDE_TIMEOUT; i++) {
        uint8_t status = inb(device->io_base + IDE_REG_STATUS);
        if (status & (IDE_STATUS_ERROR | IDE_STATUS_FAULT)) return -EIO;
        if (!(status & IDE_STATUS_BUSY) && (status & IDE_STATUS_DRQ)) return 0;
        __asm__ volatile ("pause");
    }
    return -ETIMEDOUT;
}

bool make_ide_disk_name(char *name, size_t name_size, const char *prefix, uint64_t index) {
    if (!name || !name_size || !prefix) return false;
    size_t prefix_size = 0;
    while (prefix[prefix_size]) prefix_size++;
    char suffix[16];
    size_t suffix_size = 0;
    do {
        if (suffix_size == sizeof(suffix)) return false;
        suffix[suffix_size++] = 'a' + index % 26;
        if (index < 26) break;
        index = index / 26 - 1;
    } while (true);
    if (prefix_size + suffix_size + 1 > name_size) return false;
    for (size_t i = 0; i < prefix_size; i++) name[i] = prefix[i];
    for (size_t i = 0; i < suffix_size; i++) name[prefix_size + i] = suffix[suffix_size - i - 1];
    name[prefix_size + suffix_size] = '\0';
    return true;
}

bool make_ide_numbered_name(char *name, size_t name_size, const char *prefix, uint64_t index) {
    if (!name || !name_size || !prefix) return false;
    size_t prefix_size = 0;
    while (prefix[prefix_size]) prefix_size++;
    char suffix[20];
    size_t suffix_size = 0;
    do {
        suffix[suffix_size++] = '0' + index % 10;
        index /= 10;
    } while (index);
    if (prefix_size + suffix_size + 1 > name_size) return false;
    for (size_t i = 0; i < prefix_size; i++) name[i] = prefix[i];
    for (size_t i = 0; i < suffix_size; i++) name[prefix_size + i] = suffix[suffix_size - i - 1];
    name[prefix_size + suffix_size] = '\0';
    return true;
}

void get_ide_device(uint8_t index, ide_device_t *device) {
    uint8_t channel = index / 2;
    device->io_base = ide_channels[channel][0];
    device->control_base = ide_channels[channel][1];
    device->bus_master_base = ide_channels[channel][2];
    device->slave = index & 1;
}

int prepare_ide_dma(const ide_device_t *device, uint32_t bytes, bool read) {
    if (!ide_ready || !bytes || bytes > IDE_DMA_BUFFER_SIZE) return -EINVAL;
    uint64_t data_phys = virt_to_phys(ide_dma_data);
    uint64_t prdt_phys = virt_to_phys(ide_prdt);
    if (data_phys > UINT32_MAX || bytes > UINT32_MAX - data_phys || prdt_phys > UINT32_MAX || (prdt_phys & 0xFFFF) > 0xFFF0) return -EIO;

    uint32_t first = 0x10000 - (uint32_t)(data_phys & 0xFFFF);
    if (first > bytes) first = bytes;
    ide_prdt[0].address = (uint32_t)data_phys;
    ide_prdt[0].byte_count = first == 0x10000 ? 0 : (uint16_t)first;
    ide_prdt[0].flags = first == bytes ? 0x8000 : 0;
    if (first < bytes) {
        uint32_t second = bytes - first;
        ide_prdt[1].address = (uint32_t)(data_phys + first);
        ide_prdt[1].byte_count = second == 0x10000 ? 0 : (uint16_t)second;
        ide_prdt[1].flags = 0x8000;
    }

    uint8_t command = inb(device->bus_master_base + IDE_BM_COMMAND) & ~(IDE_BM_START | IDE_BM_READ);
    if (read) command |= IDE_BM_READ;
    outb(device->bus_master_base + IDE_BM_COMMAND, command);
    outb(device->bus_master_base + IDE_BM_STATUS, inb(device->bus_master_base + IDE_BM_STATUS) | IDE_BM_ERROR | IDE_BM_INTERRUPT);
    outl(device->bus_master_base + IDE_BM_PRDT, (uint32_t)prdt_phys);
    return 0;
}

int start_ide_dma(const ide_device_t *device) {
    uint8_t command = inb(device->bus_master_base + IDE_BM_COMMAND) & ~IDE_BM_START;
    __asm__ volatile ("mfence" ::: "memory");
    outb(device->bus_master_base + IDE_BM_COMMAND, command | IDE_BM_START);

    int result = -ETIMEDOUT;
    for (uint32_t i = 0; i < IDE_TIMEOUT; i++) {
        uint8_t status = inb(device->bus_master_base + IDE_BM_STATUS);
        if (status & IDE_BM_ERROR) {
            result = -EIO;
            break;
        }
        if (!(status & IDE_BM_ACTIVE)) {
            result = 0;
            break;
        }
        __asm__ volatile ("pause");
    }

    outb(device->bus_master_base + IDE_BM_COMMAND, command);
    outb(device->bus_master_base + IDE_BM_STATUS, inb(device->bus_master_base + IDE_BM_STATUS) | IDE_BM_ERROR | IDE_BM_INTERRUPT);
    __asm__ volatile ("mfence" ::: "memory");
    if (result == 0) result = wait_ide_not_busy(device);
    return result;
}

static void detect_ide_devices(void) {
    for (uint8_t i = 0; i < IDE_MAX_DEVICES; i++) {
        ide_device_t device;
        get_ide_device(i, &device);
        select_ide_device(&device);
        outb(device.io_base + IDE_REG_SECTOR_COUNT, 0);
        outb(device.io_base + IDE_REG_LBA_LOW, 0);
        outb(device.io_base + IDE_REG_LBA_MID, 0);
        outb(device.io_base + IDE_REG_LBA_HIGH, 0);
        outb(device.io_base + IDE_REG_COMMAND, 0xEC);

        uint8_t status = inb(device.io_base + IDE_REG_STATUS);
        if (!status || status == 0xFF) continue;
        for (uint32_t timeout = 0; timeout < IDE_TIMEOUT && (status & IDE_STATUS_BUSY); timeout++) {
            __asm__ volatile ("pause");
            status = inb(device.io_base + IDE_REG_STATUS);
        }
        if (status & IDE_STATUS_BUSY) continue;

        uint8_t signature_mid = inb(device.io_base + IDE_REG_LBA_MID);
        uint8_t signature_high = inb(device.io_base + IDE_REG_LBA_HIGH);
        if (!signature_mid && !signature_high && !(status & (IDE_STATUS_ERROR | IDE_STATUS_FAULT)) && wait_ide_drq(&device) == 0) {
            for (int word = 0; word < 256; word++) inw(device.io_base + IDE_REG_DATA);
            is_pata_present = true;
        } else if ((signature_mid == 0x14 && signature_high == 0xEB) || (signature_mid == 0x69 && signature_high == 0x96)) {
            is_atapi_present = true;
        }
    }
}

bool init_ide(pci_device_t *dev) {
    if (!dev) return false;
    if (ide_ready) return dev->bus == ide_bus && dev->dev == ide_dev && dev->func == ide_func;

    set_pci_d0(dev);
    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);
    uint32_t bar2 = read_pci(dev->bus, dev->dev, dev->func, 0x18);
    uint32_t bar3 = read_pci(dev->bus, dev->dev, dev->func, 0x1C);
    uint32_t bar4 = read_pci(dev->bus, dev->dev, dev->func, 0x20);
    if (!(bar4 & 0x1) || !(bar4 & 0xFFFFFFFCu)) return false;

    uint16_t bus_master = (uint16_t)(bar4 & 0xFFFCu);
    ide_channels[0][0] = bar0 & 0xFFFFFFFCu ? (uint16_t)(bar0 & 0xFFFCu) : IDE_PRIMARY_IO;
    ide_channels[0][1] = bar1 & 0xFFFFFFFCu ? (uint16_t)((bar1 & 0xFFFCu) + 2) : IDE_PRIMARY_CONTROL;
    ide_channels[0][2] = bus_master;
    ide_channels[1][0] = bar2 & 0xFFFFFFFCu ? (uint16_t)(bar2 & 0xFFFCu) : IDE_SECONDARY_IO;
    ide_channels[1][1] = bar3 & 0xFFFFFFFCu ? (uint16_t)((bar3 & 0xFFFCu) + 2) : IDE_SECONDARY_CONTROL;
    ide_channels[1][2] = bus_master + 8;

    uint32_t command = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, command | 0x5);
    outb(ide_channels[0][1], 0x02);
    outb(ide_channels[1][1], 0x02);

    uint64_t data_phys = virt_to_phys(ide_dma_data);
    uint64_t prdt_phys = virt_to_phys(ide_prdt);
    if (data_phys > UINT32_MAX || IDE_DMA_BUFFER_SIZE > UINT32_MAX - data_phys || prdt_phys > UINT32_MAX || (prdt_phys & 0xFFFF) > 0xFFF0) return false;

    ide_bus = dev->bus;
    ide_dev = dev->dev;
    ide_func = dev->func;
    ide_ready = true;
    detect_ide_devices();
    log("ide: initialized ide\n");
    return true;
}
