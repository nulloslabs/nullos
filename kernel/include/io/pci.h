#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_PCI_DEVICES 256
#define LEGACY_IRQ_BASE 32
#define MSI_VECTOR_BASE 48
#define MSI_VECTOR_END 96
#define MAX_INTX_SHARED 8
#define PCIE_CFG_SIZE 4096
#define PCIE_ECAM_BYTES_PER_BUS 1048576
#define PCIE_ECAM_STRIDE 4096
#define PCIE_EXT_OFFSET 256

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class, subclass, progif;
} pci_device_t;

typedef struct {
    void (*fns[MAX_INTX_SHARED])(void);
    int count;
} intx_chain_t;

typedef struct {
    uint64_t base;
    uint16_t seg;
    uint8_t start_bus;
    uint8_t end_bus;
} pcie_ecam_entry_t;

extern pci_device_t pci_devices[MAX_PCI_DEVICES];
extern int pci_device_count;

bool has_pcie_ecam(void);
uint64_t get_pcie_ecam_base(void);
uint32_t read_pcie_ecam(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void write_pcie_ecam(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t val);
uint16_t read_pcie_word(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void write_pcie_word(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t val);
uint16_t find_pcie_ext_cap(uint8_t bus, uint8_t dev, uint8_t func, uint16_t cap_id);
void pci_dispatch(uint8_t vector);
void pci_register_msi_handler(uint8_t vector, void (*handler)(void));
void pci_register_intx_handler(uint8_t irq_line, void (*handler)(void));
uint32_t read_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void write_pci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);
uint16_t read_pci_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void write_pci_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val);
uint16_t vendor_pci(uint8_t bus, uint8_t dev, uint8_t func);
pci_device_t* find_pci(uint16_t vendor, uint16_t device);
pci_device_t* find_pci_class(uint8_t class, uint8_t subclass, uint8_t progif);
void set_pci_d0(pci_device_t *dev);
uint8_t pci_get_intx_vector(pci_device_t *dev);
uint8_t pci_enable_msi(pci_device_t *dev);
uint8_t pci_request_irq(pci_device_t *dev, void (*handler)(void));
void init_pci(void);
void init_pci_drivers(void);
