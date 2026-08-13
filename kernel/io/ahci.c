#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/ahci.h>
#include <io/sata.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define AHCI_GHC_AE              (1U << 31)
#define AHCI_CAP_S64A            (1U << 31)
#define AHCI_CAP2_BOH            (1U << 0)
#define AHCI_BOHC_BOS            (1U << 0)
#define AHCI_BOHC_OOS            (1U << 1)
#define AHCI_BOHC_BB             (1U << 4)
#define AHCI_PORT_CMD_ST         (1U << 0)
#define AHCI_PORT_CMD_FRE        (1U << 4)
#define AHCI_PORT_CMD_FR         (1U << 14)
#define AHCI_PORT_CMD_CR         (1U << 15)
#define AHCI_PORT_TFD_BSY        (1U << 7)
#define AHCI_PORT_TFD_DRQ        (1U << 3)
#define AHCI_PORT_IS_TFES        (1U << 30)
#define AHCI_SSTS_DET_PRESENT    3
#define AHCI_SSTS_IPM_ACTIVE     1
#define AHCI_SIG_ATA             0x00000101
#define AHCI_FIS_TYPE_REG_H2D    0x27
#define AHCI_ATA_IDENTIFY        0xEC
#define AHCI_ATA_READ_DMA_EXT    0x25
#define AHCI_ATA_WRITE_DMA_EXT   0x35
#define AHCI_ATA_FLUSH_CACHE_EXT 0xEA
#define AHCI_TIMEOUT             10000000U
#define AHCI_COMMAND_PAGE_CLB    0
#define AHCI_COMMAND_PAGE_FB     1024
#define AHCI_COMMAND_PAGE_CTBA   1280
#define AHCI_PRDT_DBC_IOC        (1U << 31)

typedef struct {
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
} ahci_port_regs_t;

typedef struct {
    volatile uint32_t cap;
    volatile uint32_t ghc;
    volatile uint32_t is;
    volatile uint32_t pi;
    volatile uint32_t vs;
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;
    volatile uint32_t bohc;
    volatile uint8_t reserved[0x74];
    volatile uint8_t vendor[0x60];
    ahci_port_regs_t ports[32];
} ahci_hba_regs_t;

typedef struct {
    uint16_t flags;
    uint16_t prdt_length;
    volatile uint32_t transferred;
    uint32_t table_base;
    uint32_t table_base_upper;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_command_header_t;

typedef struct {
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved;
    uint32_t byte_count;
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt[1];
} __attribute__((packed)) ahci_command_table_t;

typedef struct {
    uint8_t fis_type;
    uint8_t flags;
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t feature_high;
    uint8_t count_low;
    uint8_t count_high;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __attribute__((packed)) ahci_fis_h2d_t;

typedef struct {
    ahci_port_regs_t *regs;
    uint64_t command_phys;
    uint8_t *command_virt;
    uint64_t data_phys;
    uint8_t *data_virt;
    uint64_t sectors;
    char model[41];
    spinlock_t lock;
    bool ready;
} ahci_device_t;

_Static_assert(sizeof(ahci_port_regs_t) == 0x80, "invalid ahci port register layout");
_Static_assert(offsetof(ahci_hba_regs_t, ports) == 0x100, "invalid ahci hba register layout");
_Static_assert(sizeof(ahci_command_header_t) == 32, "invalid ahci command header layout");

static ahci_device_t ahci_devices[AHCI_MAX_DEVICES];
static int ahci_devices_found;

static bool ahci_wait_clear(volatile uint32_t *reg, uint32_t mask) {
    for (uint32_t timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
        if (!(*reg & mask)) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool ahci_stop_port(ahci_port_regs_t *port) {
    port->cmd &= ~AHCI_PORT_CMD_ST;
    if (!ahci_wait_clear(&port->cmd, AHCI_PORT_CMD_CR)) return false;
    port->cmd &= ~AHCI_PORT_CMD_FRE;
    return ahci_wait_clear(&port->cmd, AHCI_PORT_CMD_FR);
}

static void ahci_start_port(ahci_port_regs_t *port) {
    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;
}

static bool ahci_port_has_sata(ahci_port_regs_t *port) {
    uint32_t status = port->ssts;
    uint8_t det = status & 0x0F;
    uint8_t ipm = (status >> 8) & 0x0F;
    return det == AHCI_SSTS_DET_PRESENT && ipm == AHCI_SSTS_IPM_ACTIVE && port->sig == AHCI_SIG_ATA;
}

static int ahci_issue(ahci_device_t *device, uint8_t command, uint64_t lba, uint16_t sectors, uint32_t bytes, bool write) {
    ahci_port_regs_t *port = device->regs;
    if (!ahci_wait_clear(&port->tfd, AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) return -ETIMEDOUT;

    ahci_command_header_t *headers = (ahci_command_header_t *)(device->command_virt + AHCI_COMMAND_PAGE_CLB);
    ahci_command_table_t *table = (ahci_command_table_t *)(device->command_virt + AHCI_COMMAND_PAGE_CTBA);
    memset(&headers[0], 0, sizeof(headers[0]));
    memset(table, 0, sizeof(*table));
    headers[0].flags = sizeof(ahci_fis_h2d_t) / sizeof(uint32_t);
    if (write) headers[0].flags |= 1U << 6;
    headers[0].prdt_length = bytes ? 1 : 0;
    headers[0].table_base = (uint32_t)(device->command_phys + AHCI_COMMAND_PAGE_CTBA);
    headers[0].table_base_upper = (uint32_t)((device->command_phys + AHCI_COMMAND_PAGE_CTBA) >> 32);
    if (bytes) {
        table->prdt[0].data_base = (uint32_t)device->data_phys;
        table->prdt[0].data_base_upper = (uint32_t)(device->data_phys >> 32);
        table->prdt[0].byte_count = (bytes - 1) | AHCI_PRDT_DBC_IOC;
    }

    ahci_fis_h2d_t *fis = (ahci_fis_h2d_t *)table->command_fis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->flags = 1U << 7;
    fis->command = command;
    fis->device = 1U << 6;
    fis->lba0 = lba;
    fis->lba1 = lba >> 8;
    fis->lba2 = lba >> 16;
    fis->lba3 = lba >> 24;
    fis->lba4 = lba >> 32;
    fis->lba5 = lba >> 40;
    fis->count_low = sectors;
    fis->count_high = sectors >> 8;

    port->is = UINT32_MAX;
    __asm__ volatile ("mfence" ::: "memory");
    port->ci = 1;
    for (uint32_t timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
        if (port->is & AHCI_PORT_IS_TFES) return -EIO;
        if (!(port->ci & 1)) {
            __asm__ volatile ("mfence" ::: "memory");
            return port->tfd & 1 ? -EIO : 0;
        }
        __asm__ volatile ("pause");
    }
    return -ETIMEDOUT;
}

static int ahci_identify(ahci_device_t *device) {
    int status = ahci_issue(device, AHCI_ATA_IDENTIFY, 0, 0, AHCI_SECTOR_SIZE, false);
    if (status < 0) return status;
    uint16_t *identify = (uint16_t *)device->data_virt;
    if (!(identify[83] & (1U << 10))) return -EOPNOTSUPP;
    device->sectors = (uint64_t)identify[100] | ((uint64_t)identify[101] << 16) | ((uint64_t)identify[102] << 32) | ((uint64_t)identify[103] << 48);
    if (!device->sectors) return -ENODEV;
    for (int word = 0; word < 20; word++) {
        uint16_t value = identify[27 + word];
        device->model[word * 2] = value >> 8;
        device->model[word * 2 + 1] = value;
    }
    device->model[40] = '\0';
    for (int end = 39; end >= 0 && device->model[end] == ' '; end--) device->model[end] = '\0';
    return 0;
}

static bool ahci_prepare_device(ahci_hba_regs_t *hba, int port_index, bool dma64) {
    if (ahci_devices_found >= AHCI_MAX_DEVICES) return false;
    ahci_port_regs_t *port = &hba->ports[port_index];
    if (!ahci_port_has_sata(port) || !ahci_stop_port(port)) return false;

    void *command_phys_ptr = dma64 ? pmalloc() : pmalloc_dma32();
    void *data_phys_ptr = dma64 ? prealloc(AHCI_MAX_TRANSFER_SIZE / PAGE_SIZE) : prealloc_dma32(AHCI_MAX_TRANSFER_SIZE / PAGE_SIZE);
    if (!command_phys_ptr || !data_phys_ptr) {
        if (command_phys_ptr) pfree(command_phys_ptr);
        if (data_phys_ptr) pfree_range(data_phys_ptr, AHCI_MAX_TRANSFER_SIZE);
        return false;
    }

    ahci_device_t *device = &ahci_devices[ahci_devices_found];
    memset(device, 0, sizeof(*device));
    device->regs = port;
    device->command_phys = (uint64_t)command_phys_ptr;
    device->command_virt = phys_to_virt(device->command_phys);
    device->data_phys = (uint64_t)data_phys_ptr;
    device->data_virt = phys_to_virt(device->data_phys);
    device->lock = SPINLOCK_INIT;
    memset(device->command_virt, 0, PAGE_SIZE);
    port->clb = (uint32_t)device->command_phys;
    port->clbu = device->command_phys >> 32;
    port->fb = (uint32_t)(device->command_phys + AHCI_COMMAND_PAGE_FB);
    port->fbu = (device->command_phys + AHCI_COMMAND_PAGE_FB) >> 32;
    port->serr = UINT32_MAX;
    port->is = UINT32_MAX;
    port->ie = 0;
    ahci_start_port(port);
    if (ahci_identify(device) < 0) {
        ahci_stop_port(port);
        pfree(command_phys_ptr);
        pfree_range(data_phys_ptr, AHCI_MAX_TRANSFER_SIZE);
        memset(device, 0, sizeof(*device));
        return false;
    }
    device->ready = true;
    ahci_devices_found++;
    log("ahci: sata port %d, %s, %llu sectors\n", port_index, device->model, device->sectors);
    return true;
}

int ahci_device_count(void) { return ahci_devices_found; }

bool ahci_device_info(int index, ahci_device_info_t *info) {
    if (index < 0 || index >= ahci_devices_found || !info || !ahci_devices[index].ready) return false;
    info->sectors = ahci_devices[index].sectors;
    info->sector_size = AHCI_SECTOR_SIZE;
    memcpy(info->model, ahci_devices[index].model, sizeof(info->model));
    return true;
}

int ahci_read_sectors(int index, uint64_t lba, uint32_t sectors, void *buffer) {
    if (index < 0 || index >= ahci_devices_found || !buffer || !sectors) return -EINVAL;
    ahci_device_t *device = &ahci_devices[index];
    if (!device->ready || lba >= device->sectors || sectors > device->sectors - lba || sectors > AHCI_MAX_TRANSFER_SIZE / AHCI_SECTOR_SIZE) return -EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&device->lock, &flags);
    int status = ahci_issue(device, AHCI_ATA_READ_DMA_EXT, lba, sectors, sectors * AHCI_SECTOR_SIZE, false);
    if (status == 0) memcpy(buffer, device->data_virt, sectors * AHCI_SECTOR_SIZE);
    spin_unlock_irqrestore(&device->lock, flags);
    return status;
}

int ahci_write_sectors(int index, uint64_t lba, uint32_t sectors, const void *buffer) {
    if (index < 0 || index >= ahci_devices_found || !buffer || !sectors) return -EINVAL;
    ahci_device_t *device = &ahci_devices[index];
    if (!device->ready || lba >= device->sectors || sectors > device->sectors - lba || sectors > AHCI_MAX_TRANSFER_SIZE / AHCI_SECTOR_SIZE) return -EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&device->lock, &flags);
    memcpy(device->data_virt, buffer, sectors * AHCI_SECTOR_SIZE);
    int status = ahci_issue(device, AHCI_ATA_WRITE_DMA_EXT, lba, sectors, sectors * AHCI_SECTOR_SIZE, true);
    if (status == 0) status = ahci_issue(device, AHCI_ATA_FLUSH_CACHE_EXT, 0, 0, 0, false);
    spin_unlock_irqrestore(&device->lock, flags);
    return status;
}

bool init_ahci(pci_device_t *dev) {
    if (!dev) return false;
    set_pci_d0(dev);
    uint32_t bar_low = read_pci(dev->bus, dev->dev, dev->func, 0x24);
    if (bar_low & 1) return false;
    uint64_t abar_phys = bar_low & 0xFFFFFFF0U;
    if ((bar_low & 0x06) == 0x04) return false;
    if (!abar_phys) return false;

    uint64_t page_offset = abar_phys & (PAGE_SIZE - 1);
    size_t pages = (page_offset + sizeof(ahci_hba_regs_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    ahci_hba_regs_t *hba = vmap_mmio(abar_phys, pages);
    if (!hba) return false;
    uint32_t command = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, command | 0x06);
    if (hba->cap2 & AHCI_CAP2_BOH) {
        hba->bohc |= AHCI_BOHC_OOS;
        for (uint32_t timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
            if (!(hba->bohc & (AHCI_BOHC_BOS | AHCI_BOHC_BB))) break;
            __asm__ volatile ("pause");
        }
    }
    hba->ghc |= AHCI_GHC_AE;
    uint32_t implemented = hba->pi;
    bool dma64 = hba->cap & AHCI_CAP_S64A;
    for (int port = 0; port < 32 && ahci_devices_found < AHCI_MAX_DEVICES; port++) {
        if (implemented & (1U << port)) ahci_prepare_device(hba, port, dma64);
    }
    if (!ahci_devices_found) return false;
    is_sata_present = true;
    log("ahci: initialized ahci with %d sata device(s)\n", ahci_devices_found);
    return true;
}
