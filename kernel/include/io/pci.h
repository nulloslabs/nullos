#pragma once

#include <freestanding/stdint.h>

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class, subclass, progif;
} pci_device_t;

extern pci_device_t pci_devices[256];
extern int pci_device_count;
extern void (*irq43_handlers[4])(void);
extern int irq43_count;

void register_pci_interrupt_handler(void (*handler)(void));
void handle_pci_interrupt(void);
uint32_t read_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void write_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);
uint16_t vendor_pci(uint8_t bus, uint8_t dev, uint8_t func);
pci_device_t* find_pci(uint16_t vendor, uint16_t device);
pci_device_t* find_pci_class(uint8_t class, uint8_t subclass, uint8_t progif);
void set_pci_d0(pci_device_t *dev);
void init_pci(void);
void init_pci_drivers(void);