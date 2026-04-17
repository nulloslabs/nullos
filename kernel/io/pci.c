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
#include <io/ohci.h>
#include <io/ehci.h>
#include <io/xhci.h>

pci_device_t pci_devices[256];
int pci_device_count = 0;
void (*irq43_handlers[4])(void) = { 0 };
int irq43_count = 0;

void register_pci_interrupt_handler(void (*handler)(void)) {
    if (irq43_count < 4) {
        irq43_handlers[irq43_count++] = handler;
    }
}

void handle_pci_interrupt(void) {
    for (int i = 0; i < irq43_count; i++) {
        if (irq43_handlers[i]) {
            irq43_handlers[i]();
        }
    }
}

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

uint16_t vendor_pci(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)(read_pci(bus, dev, func, 0) & 0xFFFF);
}

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

void set_pci_d0(pci_device_t *dev) {
    uint32_t status_cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    // Bit 4 of Status register is bit 20 of register 0x04
    if (!(status_cmd & (1 << 20))) {
        return; // No capabilities list
    }

    uint8_t cap_ptr = read_pci(dev->bus, dev->dev, dev->func, 0x34) & 0xFF;
    cap_ptr &= 0xFC; // Capability pointers are dword-aligned

    while (cap_ptr) {
        uint32_t cap_header = read_pci(dev->bus, dev->dev, dev->func, cap_ptr);
        uint8_t cap_id = cap_header & 0xFF;
        uint8_t next_ptr = ((cap_header >> 8) & 0xFF) & 0xFC;

        if (cap_id == 0x01) { // Power Management
            uint32_t pmcsr = read_pci(dev->bus, dev->dev, dev->func, cap_ptr + 4);
            if ((pmcsr & 0x03) != 0) { // Not in D0
                printf("PCI: Transitioning %02x:%02x.%x from D%d to D0\n", dev->bus, dev->dev, dev->func, pmcsr & 0x03);
                pmcsr &= ~0x03; // Mask out PowerState (bits 1:0) to 0 (D0)
                write_pci(dev->bus, dev->dev, dev->func, cap_ptr + 4, pmcsr);
                // The PCI spec requires a 10ms delay when transitioning from D3hot to D0
                extern void sleep(uint64_t ms);
                sleep(10);
            }
            break;
        }

        cap_ptr = next_ptr;
    }
}

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
                if (pci_device_count >= 256) return;
            }
        }
    }
    printf("PCI: Initialized PCI.\n");
}

void init_pci_drivers(void) {
    struct {
        const char *name;
        uint16_t vendor;
        uint16_t device;
        void (*init)(pci_device_t*);
    } known_pci_drivers[] = {
        {"AC97", AC97_VENDOR, AC97_DEVICE, init_ac97},
        {"RTL8139", RTL8139_VENDOR, RTL8139_DEVICE, init_rtl8139},
        {"E1000", E1000_VENDOR, E1000_DEVICE, init_e1000}
    };

    for (int i = 0; i < sizeof(known_pci_drivers)/sizeof(known_pci_drivers[0]); i++) {
        pci_device_t *dev = find_pci(known_pci_drivers[i].vendor, known_pci_drivers[i].device);
        if (dev) {
            printf("PCI: Found driver for %s.\n", known_pci_drivers[i].name);
            known_pci_drivers[i].init(dev);
        }
    }

    struct {
        const char *name;
        uint8_t progif;
        void (*init)(pci_device_t*);
    } known_usb_drivers[] = {
        {"UHCI", USB_PROGIF_UHCI, init_uhci},
        {"OHCI", USB_PROGIF_OHCI, init_ohci},
        {"EHCI", USB_PROGIF_EHCI, init_ehci},
        {"xHCI", USB_PROGIF_XHCI, init_xhci},
    };

    for (int i = 0; i < sizeof(known_usb_drivers)/sizeof(known_usb_drivers[0]); i++) {
        for (int j = 0; j < pci_device_count; j++) {
            if (pci_devices[j].class == USB_PCI_CLASS &&
                pci_devices[j].subclass == USB_PCI_SUBCLASS &&
                pci_devices[j].progif == known_usb_drivers[i].progif) {
                printf("PCI: Found %s USB controller.\n", known_usb_drivers[i].name);
                known_usb_drivers[i].init(&pci_devices[j]);
            }
        }
    }

}