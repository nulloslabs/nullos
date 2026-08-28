#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/log.h>
#include <io/pci.h>
#include <io/io.h>
#include <io/ac97.h>
#include <io/bga.h>
#include <io/e1000.h>
#include <io/rtl8139.h>
#include <io/svga_ii.h>
#include <io/virtio_gpu.h>
#include <io/ide.h>
#include <io/ahci.h>
#include <io/atapi.h>
#include <io/pata.h>
#include <io/sata.h>
#include <io/usb.h>
#include <io/uhci.h>
#include <io/ohci.h>
#include <io/time.h>
#include <io/nvme.h>
#include <mm/vmm.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/status.h>

static void (*msi_handlers[256])(void) = {0};
static intx_chain_t intx_chains[16] = {0};
static uint8_t next_msi_vector = MSI_VECTOR_BASE;
static volatile uint8_t *g_ecam_virt = 0;
static uint64_t g_ecam_phys = 0;
static uint8_t g_ecam_start = 0;
static uint8_t g_ecam_end = 0;
static bool g_has_ecam = false;
static size_t g_ecam_pages = 0;

pci_device_t pci_devices[MAX_PCI_DEVICES];
int pci_device_count = 0;

static bool is_ecam_covered(uint8_t bus) {
    if (!g_has_ecam) return false;
    if (bus < g_ecam_start) return false;
    if (bus > g_ecam_end) return false;
    return true;
}

static volatile uint8_t* get_ecam_ptr(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    // calc ecam offset
    uint32_t bus_off = (uint32_t)(bus - g_ecam_start) * PCIE_ECAM_BYTES_PER_BUS;
    uint32_t dev_off = (uint32_t)dev * 32768;
    uint32_t func_off = (uint32_t)func * PCIE_ECAM_STRIDE;
    uint32_t total = bus_off + dev_off + func_off + off;
    return g_ecam_virt + total;
}

static int init_pcie_ecam(void) {
    // find mcfg via uacpi
    uacpi_table tbl;
    uacpi_status st = uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &tbl);
    if (uacpi_unlikely_error(st)) return -1;
    struct acpi_mcfg *mcfg = (struct acpi_mcfg*)tbl.hdr;
    if (!mcfg) { uacpi_table_unref(&tbl); return -1; }
    uint32_t hdr_len = mcfg->hdr.length;
    if (hdr_len < sizeof(struct acpi_mcfg)) { uacpi_table_unref(&tbl); return -1; }
    uint32_t entry_count = (hdr_len - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_allocation);
    if (entry_count == 0) { uacpi_table_unref(&tbl); return -1; }
    // use first entry with seg 0
    struct acpi_mcfg_allocation *chosen = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_mcfg_allocation *e = &mcfg->entries[i];
        if (e->segment == 0) { chosen = e; break; }
    }
    if (!chosen) { uacpi_table_unref(&tbl); return -1; }
    if (chosen->start_bus > chosen->end_bus) { uacpi_table_unref(&tbl); return -1; }
    uint64_t base = chosen->address;
    uint8_t start = chosen->start_bus;
    uint8_t end = chosen->end_bus;
    uint64_t bus_cnt = (uint64_t)end - start + 1;
    uint64_t bytes = bus_cnt * PCIE_ECAM_BYTES_PER_BUS;
    size_t pages = (bytes + 4095) / 4096;
    void *virt = vmap_mmio(base, pages);
    if (!virt) { uacpi_table_unref(&tbl); return -1; }
    g_ecam_virt = (volatile uint8_t*)virt;
    g_ecam_phys = base;
    g_ecam_start = start;
    g_ecam_end = end;
    g_has_ecam = true;
    g_ecam_pages = pages;
    uacpi_table_unref(&tbl);
    return 0;
}

static uint8_t pci_find_cap(pci_device_t *dev, uint8_t cap_id) {
    uint32_t status_cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    if (!(status_cmd & (1 << 20))) return 0;
    uint8_t ptr = read_pci(dev->bus, dev->dev, dev->func, 0x34) & 0xFC;
    while (ptr) {
        uint32_t hdr = read_pci(dev->bus, dev->dev, dev->func, ptr);
        if ((hdr & 0xFF) == cap_id) return ptr;
        ptr = ((hdr >> 8) & 0xFF) & 0xFC;
    }
    return 0;
}

static void pci_set_intx_disable(pci_device_t *dev, int disable) {
    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    if (disable) cmd |= (1 << 10);
    else cmd &= ~(1 << 10);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd);
}

bool has_pcie_ecam(void) {
    return g_has_ecam;
}

uint64_t get_pcie_ecam_base(void) {
    return g_ecam_phys;
}

uint32_t read_pcie_ecam(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    if (!g_has_ecam) return 0xFFFFFFFF;
    if (!is_ecam_covered(bus)) return 0xFFFFFFFF;
    if (off >= PCIE_CFG_SIZE) return 0xFFFFFFFF;
    if (off & 3) return 0xFFFFFFFF;
    volatile uint32_t *p = (volatile uint32_t*)get_ecam_ptr(bus, dev, func, off);
    return *p;
}

void write_pcie_ecam(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t val) {
    if (!g_has_ecam) return;
    if (!is_ecam_covered(bus)) return;
    if (off >= PCIE_CFG_SIZE) return;
    if (off & 3) return;
    volatile uint32_t *p = (volatile uint32_t*)get_ecam_ptr(bus, dev, func, off);
    *p = val;
}

uint16_t read_pcie_word(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    if (!g_has_ecam) return 0xFFFF;
    if (!is_ecam_covered(bus)) return 0xFFFF;
    if (off >= PCIE_CFG_SIZE - 1) return 0xFFFF;
    if (off & 1) return 0xFFFF;
    volatile uint16_t *p = (volatile uint16_t*)get_ecam_ptr(bus, dev, func, off);
    return *p;
}

void write_pcie_word(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t val) {
    if (!g_has_ecam) return;
    if (!is_ecam_covered(bus)) return;
    if (off >= PCIE_CFG_SIZE - 1) return;
    if (off & 1) return;
    volatile uint16_t *p = (volatile uint16_t*)get_ecam_ptr(bus, dev, func, off);
    *p = val;
}

uint16_t find_pcie_ext_cap(uint8_t bus, uint8_t dev, uint8_t func, uint16_t cap_id) {
    uint16_t ptr = PCIE_EXT_OFFSET;
    while (ptr) {
        if (ptr + 4 > PCIE_CFG_SIZE) return 0;
        uint32_t hdr = read_pcie_ecam(bus, dev, func, ptr);
        if (hdr == 0xFFFFFFFF) return 0;
        if (hdr == 0) return 0;
        uint16_t id = hdr & 0xFFFF;
        uint16_t next = (hdr >> 20) & 0xFFF;
        if (id == cap_id) return ptr;
        if (next <= ptr) return 0;
        ptr = next;
    }
    return 0;
}

void pci_dispatch(uint8_t vector) {
    if (vector >= LEGACY_IRQ_BASE && vector < LEGACY_IRQ_BASE + 16) {
        intx_chain_t *c = &intx_chains[vector - LEGACY_IRQ_BASE];
        for (int i = 0; i < c->count; i++) {
            if (c->fns[i]) c->fns[i]();
        }
        return;
    }
    void (*h)(void) = msi_handlers[vector];
    if (h) h();
}

void pci_register_msi_handler(uint8_t vector, void (*handler)(void)) {
    msi_handlers[vector] = handler;
}

void pci_register_intx_handler(uint8_t irq_line, void (*handler)(void)) {
    if (irq_line >= 16) return;
    intx_chain_t *c = &intx_chains[irq_line];
    if (c->count < MAX_INTX_SHARED) c->fns[c->count++] = handler;
}

uint32_t read_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    if (g_has_ecam && is_ecam_covered(bus)) {
        if ((reg & 3) == 0) {
            volatile uint32_t *p = (volatile uint32_t*)get_ecam_ptr(bus, dev, func, reg);
            return *p;
        }
        // unaligned fallback via ecam
        volatile uint8_t *base = get_ecam_ptr(bus, dev, func, reg & 0xFC);
        uint32_t v = *(volatile uint32_t*)base;
        return v;
    }
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC));
    return inl(0xCFC);
}

void write_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val) {
    if (g_has_ecam && is_ecam_covered(bus)) {
        if ((reg & 3) == 0) {
            volatile uint32_t *p = (volatile uint32_t*)get_ecam_ptr(bus, dev, func, reg);
            *p = val;
            return;
        }
        volatile uint32_t *p = (volatile uint32_t*)get_ecam_ptr(bus, dev, func, reg & 0xFC);
        *p = val;
        return;
    }
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC));
    outl(0xCFC, val);
}

uint16_t read_pci_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    if (g_has_ecam && is_ecam_covered(bus)) {
        volatile uint16_t *p = (volatile uint16_t*)get_ecam_ptr(bus, dev, func, reg);
        return *p;
    }
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC));
    return inw(0xCFC + (reg & 2));
}

void write_pci_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val) {
    if (g_has_ecam && is_ecam_covered(bus)) {
        volatile uint16_t *p = (volatile uint16_t*)get_ecam_ptr(bus, dev, func, reg);
        *p = val;
        return;
    }
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC));
    outw(0xCFC + (reg & 2), val);
}

uint16_t vendor_pci(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)(read_pci(bus, dev, func, 0) & 0xFFFF);
}

pci_device_t* find_pci(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor == vendor && pci_devices[i].device == device) return &pci_devices[i];
    }
    return 0;
}

pci_device_t* find_pci_class(uint8_t class, uint8_t subclass, uint8_t progif) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class == class && pci_devices[i].subclass == subclass && pci_devices[i].progif == progif) return &pci_devices[i];
    }
    return 0;
}

void set_pci_d0(pci_device_t *dev) {
    uint8_t cap = pci_find_cap(dev, 0x01);
    if (!cap) return;
    uint32_t pmcsr = read_pci(dev->bus, dev->dev, dev->func, cap + 4);
    if ((pmcsr & 0x03) != 0) {
        pmcsr &= ~0x03;
        write_pci(dev->bus, dev->dev, dev->func, cap + 4, pmcsr);
        sleep(10);
    }
}

uint8_t pci_get_intx_vector(pci_device_t *dev) {
    uint32_t r = read_pci(dev->bus, dev->dev, dev->func, 0x3C);
    uint8_t line = r & 0xFF;
    if (line == 0xFF) return 0;
    return LEGACY_IRQ_BASE + line;
}

uint8_t pci_enable_msi(pci_device_t *dev) {
    uint8_t cap = pci_find_cap(dev, 0x05);
    if (!cap) return 0;
    if (next_msi_vector >= MSI_VECTOR_END) {
        log("pci: out of msi vectors\n");
        return 0;
    }
    uint8_t vector = next_msi_vector++;
    uint32_t mc_dword = read_pci(dev->bus, dev->dev, dev->func, cap);
    uint16_t mc = (mc_dword >> 16) & 0xFFFF;
    int is_64bit = mc & (1 << 7);
    uint32_t addr_lo = 0xFEE00000u;
    uint32_t addr_hi = 0;
    uint32_t data = vector;
    write_pci(dev->bus, dev->dev, dev->func, cap + 0x04, addr_lo);
    if (is_64bit) {
        write_pci(dev->bus, dev->dev, dev->func, cap + 0x08, addr_hi);
        write_pci(dev->bus, dev->dev, dev->func, cap + 0x0C, data & 0xFFFF);
    } else {
        write_pci(dev->bus, dev->dev, dev->func, cap + 0x08, data & 0xFFFF);
    }
    mc |= 1;
    mc &= ~(7 << 4);
    uint32_t new_mc_dword = (mc_dword & 0x0000FFFF) | ((uint32_t)mc << 16);
    write_pci(dev->bus, dev->dev, dev->func, cap, new_mc_dword);
    pci_set_intx_disable(dev, 1);
    return vector;
}

uint8_t pci_request_irq(pci_device_t *dev, void (*handler)(void)) {
    uint8_t v = pci_enable_msi(dev);
    if (v) { pci_register_msi_handler(v, handler); return v; }
    pci_set_intx_disable(dev, 0);
    uint32_t r = read_pci(dev->bus, dev->dev, dev->func, 0x3C);
    uint8_t line = r & 0xFF;
    if (line == 0xFF) return 0;
    pci_register_intx_handler(line, handler);
    return LEGACY_IRQ_BASE + line;
}

void init_pci(void) {
    (void)init_pcie_ecam();
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = vendor_pci(bus, dev, func);
                if (vendor == 0xFFFF) continue;
                uint32_t id = read_pci(bus, dev, func, 0);
                uint32_t cc = read_pci(bus, dev, func, 8);
                pci_devices[pci_device_count++] = (pci_device_t){.bus = bus, .dev = dev, .func = func, .vendor = vendor, .device = (uint16_t)(id >> 16), .class = (uint8_t)(cc >> 24), .subclass = (uint8_t)(cc >> 16), .progif = (uint8_t)(cc >> 8),};
                if (pci_device_count >= MAX_PCI_DEVICES) return;
            }
        }
    }
    log("pci: initialized pci\n");
}

void init_pci_drivers(void) {
    const struct {
        const char *name;
        uint16_t vendor;
        uint16_t device;
        void (*init)(pci_device_t*);
    } known_pci_drivers[] = {
        {"ac97",       AC97_VENDOR,       AC97_DEVICE,                    init_ac97},
        {"bga",        BGA_VENDOR,        BGA_DEVICE,                     init_bga},
        {"e1000",      E1000_VENDOR,      E1000_DEVICE,                   init_e1000},
        {"rtl8139",    RTL8139_VENDOR,    RTL8139_DEVICE,                 init_rtl8139},
        {"svga ii",    SVGA_II_VENDOR,    SVGA_II_DEVICE,                 init_svga_ii},
        {"virtio-gpu", VIRTIO_GPU_VENDOR, VIRTIO_GPU_DEVICE_MODERN,       init_virtio_gpu},
        {"virtio-gpu", VIRTIO_GPU_VENDOR, VIRTIO_GPU_DEVICE_TRANSITIONAL, init_virtio_gpu},
    };

    const struct {
        const char *name;
        uint8_t class;
        uint8_t subclass;
        uint8_t progif_mask;
        uint8_t progif_value;
        bool (*init)(pci_device_t*);
    } known_storage_controllers[] = {
        {"ide",  IDE_CLASS,  IDE_SUBCLASS,  IDE_PROGIF_MASK,  IDE_PROGIF_VALUE,  init_ide},
        {"ahci", AHCI_CLASS, AHCI_SUBCLASS, AHCI_PROGIF_MASK, AHCI_PROGIF_VALUE, init_ahci},
    };

    const struct {
        const char *name;
        uint8_t progif;
        void (*init)(pci_device_t*);
    } known_usb_drivers[] = {
        {"uhci", USB_PROGIF_UHCI, init_uhci},
        {"ohci", USB_PROGIF_OHCI, init_ohci},
    };

    for (int i = 0; i < (int)(sizeof(known_pci_drivers) / sizeof(known_pci_drivers[0])); i++) {
        pci_device_t *dev = find_pci(known_pci_drivers[i].vendor, known_pci_drivers[i].device);
        if (dev) {
            log("pci: found driver for %s\n", known_pci_drivers[i].name);
            known_pci_drivers[i].init(dev);
        }
    }

    for (int i = 0; i < (int)(sizeof(known_storage_controllers) / sizeof(known_storage_controllers[0])); i++) {
        for (int j = 0; j < pci_device_count; j++) {
            pci_device_t *dev = &pci_devices[j];

            if (dev->class != known_storage_controllers[i].class) continue;
            if (dev->subclass != known_storage_controllers[i].subclass) continue;
            if ((dev->progif & known_storage_controllers[i].progif_mask) != known_storage_controllers[i].progif_value) continue;

            log("pci: found controller for %s\n", known_storage_controllers[i].name);
            if (known_storage_controllers[i].init(dev)) {
                if (dev->class == AHCI_CLASS && dev->subclass == AHCI_SUBCLASS && dev->progif == AHCI_PROGIF_VALUE) {
                    log("pci: found driver for sata\n");
                    init_sata();
                } else {
                    if (is_atapi_present) {
                        log("pci: found driver for atapi\n");
                        init_atapi();
                    }
                    if (is_pata_present) {
                        log("pci: found driver for pata\n");
                        init_pata();
                    }
                }
            }
            break;
        }
    }

    for (int i = 0; i < (int)(sizeof(known_usb_drivers)/sizeof(known_usb_drivers[0])); i++) {
        for (int j = 0; j < pci_device_count; j++) {
            if (pci_devices[j].class == USB_PCI_CLASS && pci_devices[j].subclass == USB_PCI_SUBCLASS && pci_devices[j].progif == known_usb_drivers[i].progif) {
                log("pci: found %s usb controller\n", known_usb_drivers[i].name);
                known_usb_drivers[i].init(&pci_devices[j]);
            }
        }
    }

    for (int j = 0; j < pci_device_count; j++) {
        pci_device_t *dev = &pci_devices[j];
        if (dev->class == NVME_CLASS && dev->subclass == NVME_SUBCLASS) {
            log("pci: found driver for nvme\n");
            init_nvme(dev);
        }
    }

    log("pci: initialized pci drivers\n");
}
