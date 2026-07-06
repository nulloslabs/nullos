#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/pci.h>
#include <io/io.h>
#include <io/ac97.h>
#include <io/rtl8139.h>
#include <io/e1000.h>
#include <io/terminal.h>
#include <io/usb.h>
#include <io/uhci.h>

pci_device_t pci_devices[MAX_PCI_DEVICES];
int pci_device_count = 0;

#define MAX_INTX_SHARED 8
typedef struct {
    void (*fns[MAX_INTX_SHARED])(void);
    int   count;
} intx_chain_t;

static void (*msi_handlers[256])(void) = { 0 };
static intx_chain_t intx_chains[16] = { 0 }; // legacy IRQs 0..15

static uint8_t next_msi_vector = MSI_VECTOR_BASE;

// Called from every per-vector ISR stub in pci_isr.S
void pci_dispatch(uint8_t vector) {
    if (vector >= LEGACY_IRQ_BASE && vector < LEGACY_IRQ_BASE + 16) {
        intx_chain_t *c = &intx_chains[vector - LEGACY_IRQ_BASE];
        for (int i = 0; i < c->count; i++) { if (c->fns[i]) c->fns[i](); }
        return;
    }
    void (*h)(void) = msi_handlers[vector];
    if (h) h();
}

void pci_register_msi_handler(uint8_t vector, void (*handler)(void)) { msi_handlers[vector] = handler; }

void pci_register_intx_handler(uint8_t irq_line, void (*handler)(void)) {
    if (irq_line >= 16) return;
    intx_chain_t *c = &intx_chains[irq_line];
    if (c->count < MAX_INTX_SHARED) c->fns[c->count++] = handler;
}

// ---- PCI config-space helpers -------------------------------------------
uint32_t read_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11)
                             | ((uint32_t)func<<8) | (reg & 0xFC));
    return inl(0xCFC);
}

void write_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11)
                             | ((uint32_t)func<<8) | (reg & 0xFC));
    outl(0xCFC, val);
}

uint16_t vendor_pci(uint8_t bus, uint8_t dev, uint8_t func) { return (uint16_t)(read_pci(bus, dev, func, 0) & 0xFFFF); }

pci_device_t* find_pci(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < pci_device_count; i++)
        if (pci_devices[i].vendor == vendor && pci_devices[i].device == device)
            return &pci_devices[i];
    return NULL;
}

pci_device_t* find_pci_class(uint8_t class, uint8_t subclass, uint8_t progif) {
    for (int i = 0; i < pci_device_count; i++)
        if (pci_devices[i].class == class && pci_devices[i].subclass == subclass
            && pci_devices[i].progif == progif)
            return &pci_devices[i];
    return NULL;
}

// ---- Capability-list walking --------------------------------------------
static uint8_t pci_find_cap(pci_device_t *dev, uint8_t cap_id) {
    uint32_t status_cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    if (!(status_cmd & (1 << 20))) return 0; // no caps list

    uint8_t ptr = read_pci(dev->bus, dev->dev, dev->func, 0x34) & 0xFC;
    while (ptr) {
        uint32_t hdr = read_pci(dev->bus, dev->dev, dev->func, ptr);
        if ((hdr & 0xFF) == cap_id) return ptr;
        ptr = ((hdr >> 8) & 0xFF) & 0xFC;
    }
    return 0;
}

// ---- Power management ---------------------------------------------------
void set_pci_d0(pci_device_t *dev) {
    uint8_t cap = pci_find_cap(dev, 0x01);
    if (!cap) return;

    uint32_t pmcsr = read_pci(dev->bus, dev->dev, dev->func, cap + 4);
    if ((pmcsr & 0x03) != 0) {
        printf("pci: transitioning %02x:%02x.%x from d%d to d0\n",
               dev->bus, dev->dev, dev->func, pmcsr & 0x03);
        pmcsr &= ~0x03;
        write_pci(dev->bus, dev->dev, dev->func, cap + 4, pmcsr);
        extern void sleep(uint64_t ms);
        sleep(10);
    }
}

// ---- Legacy INTx helpers ------------------------------------------------
uint8_t pci_get_intx_vector(pci_device_t *dev) {
    uint32_t r = read_pci(dev->bus, dev->dev, dev->func, 0x3C);
    uint8_t line = r & 0xFF;
    if (line == 0xFF) return 0; // not connected
    return LEGACY_IRQ_BASE + line;
}

static void pci_set_intx_disable(pci_device_t *dev, int disable) {
    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    if (disable) cmd |=  (1 << 10);
    else         cmd &= ~(1 << 10);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd);
}

uint8_t pci_enable_msi(pci_device_t *dev) {
    uint8_t cap = pci_find_cap(dev, 0x05);
    if (!cap) return 0;

    if (next_msi_vector >= MSI_VECTOR_END) { printf("pci: out of msi vectors\n"); return 0; }
    uint8_t vector = next_msi_vector++;

    // Read Message Control (upper 16 bits of cap dword 0)
    uint32_t mc_dword = read_pci(dev->bus, dev->dev, dev->func, cap);
    uint16_t mc = (mc_dword >> 16) & 0xFFFF;
    int is_64bit = mc & (1 << 7);

    // Address: deliver to BSP (LAPIC ID 0). Format: 0xFEE00000 | (apic_id<<12)
    uint32_t addr_lo = 0xFEE00000u; // edge-trigger, fixed delivery, RH=0, DM=0
    uint32_t addr_hi = 0;

    // Data: vector, fixed delivery (000), edge, assert
    uint32_t data = vector;

    write_pci(dev->bus, dev->dev, dev->func, cap + 0x04, addr_lo);
    if (is_64bit) { write_pci(dev->bus, dev->dev, dev->func, cap + 0x08, addr_hi); write_pci(dev->bus, dev->dev, dev->func, cap + 0x0C, data & 0xFFFF); } else { write_pci(dev->bus, dev->dev, dev->func, cap + 0x08, data & 0xFFFF); }

    // Set MSI Enable (bit 0 of Message Control), force MME=0 (1 vector).
    mc |= 1;          // MSI Enable
    mc &= ~(7 << 4);  // MME = 0 -> 1 message
    uint32_t new_mc_dword = (mc_dword & 0x0000FFFF) | ((uint32_t)mc << 16);
    write_pci(dev->bus, dev->dev, dev->func, cap, new_mc_dword);

    // Mask legacy INTx so it never fires for this device again.
    pci_set_intx_disable(dev, 1);

    printf("pci: %02x:%02x.%x using msi vector %d\n",
           dev->bus, dev->dev, dev->func, vector);
    return vector;
}

// One-shot helper: try MSI, fall back to legacy INTx. Returns assigned
// vector. Driver should register its handler on the returned vector.
uint8_t pci_request_irq(pci_device_t *dev, void (*handler)(void)) {
    uint8_t v = pci_enable_msi(dev);
    if (v) { pci_register_msi_handler(v, handler); return v; }
    // Fall back to legacy INTx. Make sure INTx is *enabled* (clear bit 10).
    pci_set_intx_disable(dev, 0);
    uint32_t r = read_pci(dev->bus, dev->dev, dev->func, 0x3C);
    uint8_t line = r & 0xFF;
    if (line == 0xFF) return 0;
    pci_register_intx_handler(line, handler);
    return LEGACY_IRQ_BASE + line;
}

// ---- Enumeration --------------------------------------------------------
void init_pci(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = vendor_pci(bus, dev, func);
                if (vendor == 0xFFFF) continue;
                uint32_t id = read_pci(bus, dev, func, 0);
                uint32_t cc = read_pci(bus, dev, func, 8);
                pci_devices[pci_device_count++] = (pci_device_t){
                    .bus = bus, .dev = dev, .func = func,
                    .vendor = vendor,
                    .device = (uint16_t)(id >> 16),
                    .class = (uint8_t)(cc >> 24),
                    .subclass = (uint8_t)(cc >> 16),
                    .progif = (uint8_t)(cc >> 8),
                };
                if (pci_device_count >= MAX_PCI_DEVICES) return;
            }
        }
    }
    printf("pci: initialized pci\n");
}

void init_pci_drivers(void) {
    const struct {
        const char *name;
        uint16_t vendor;
        uint16_t device;
        void (*init)(pci_device_t*);
    } known_pci_drivers[] = {
        {"ac97",    AC97_VENDOR,    AC97_DEVICE,    init_ac97},
        {"rtl8139", RTL8139_VENDOR, RTL8139_DEVICE, init_rtl8139},
        {"e1000",   E1000_VENDOR,   E1000_DEVICE,   init_e1000}
    };

    for (int i = 0; i < (int)(sizeof(known_pci_drivers) / sizeof(known_pci_drivers[0])); i++) {
        pci_device_t *dev = find_pci(known_pci_drivers[i].vendor, known_pci_drivers[i].device);
        if (dev) {
            printf("pci: found driver for %s\n", known_pci_drivers[i].name);
            known_pci_drivers[i].init(dev);
        }
    }

    const struct {
        const char *name;
        uint8_t progif;
        void (*init)(pci_device_t*);
    } known_usb_drivers[] = {
        {"uhci", USB_PROGIF_UHCI, init_uhci},
    };

    for (int i = 0; i < (int)(sizeof(known_usb_drivers)/sizeof(known_usb_drivers[0])); i++) {
        for (int j = 0; j < pci_device_count; j++) {
            if (pci_devices[j].class == USB_PCI_CLASS &&
                pci_devices[j].subclass == USB_PCI_SUBCLASS &&
                pci_devices[j].progif == known_usb_drivers[i].progif) { printf("pci: found %s usb controller\n", known_usb_drivers[i].name); known_usb_drivers[i].init(&pci_devices[j]); }
        }
    }
}
