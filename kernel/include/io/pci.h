#pragma once

#include <freestanding/stdint.h>

#define MAX_PCI_DEVICES   256
#define LEGACY_IRQ_BASE   32         // PIC/IOAPIC remap base
#define MSI_VECTOR_BASE   48         // first vector we hand out for MSI
#define MSI_VECTOR_END    96         // exclusive end (48 vectors available)

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class, subclass, progif;
} pci_device_t;

extern pci_device_t pci_devices[MAX_PCI_DEVICES];
extern int pci_device_count;

uint32_t read_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void write_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);
uint16_t vendor_pci(uint8_t bus, uint8_t dev, uint8_t func);
pci_device_t* find_pci(uint16_t vendor, uint16_t device);
pci_device_t* find_pci_class(uint8_t class, uint8_t subclass, uint8_t progif);
void set_pci_d0(pci_device_t *dev);
uint8_t pci_request_irq(pci_device_t *dev, void (*handler)(void));
uint8_t pci_enable_msi(pci_device_t *dev);
uint8_t pci_get_intx_vector(pci_device_t *dev);
void pci_register_msi_handler(uint8_t vector, void (*handler)(void));
void pci_register_intx_handler(uint8_t irq_line, void (*handler)(void));
void pci_dispatch(uint8_t vector);
void init_pci(void);
void init_pci_drivers(void);
