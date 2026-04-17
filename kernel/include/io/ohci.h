#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <io/pci.h>
#include <io/usb.h>

// ============================================================================
// OHCI Register Offsets (MMIO via BAR0)
// ============================================================================
#define OHCI_REVISION           0x00
#define OHCI_CONTROL            0x04
#define OHCI_CMDSTATUS          0x08
#define OHCI_INTRSTATUS         0x0C
#define OHCI_INTRENABLE         0x10
#define OHCI_INTRDISABLE        0x14
#define OHCI_HCCA               0x18
#define OHCI_PERIOD_CUR_ED      0x1C
#define OHCI_CTRL_HEAD_ED       0x20
#define OHCI_CTRL_CUR_ED        0x24
#define OHCI_BULK_HEAD_ED       0x28
#define OHCI_BULK_CUR_ED        0x2C
#define OHCI_DONE_HEAD          0x30
#define OHCI_FMINTERVAL         0x34
#define OHCI_FMREMAINING        0x38
#define OHCI_FMNUMBER           0x3C
#define OHCI_PERIODICSTART      0x40
#define OHCI_LSTHRESHOLD        0x44
#define OHCI_RHDESCRIPTORA      0x48
#define OHCI_RHDESCRIPTORB      0x4C
#define OHCI_RHSTATUS           0x50
#define OHCI_RHPORTSTATUS_BASE  0x54

// OHCI Control register bits
#define OHCI_CTRL_CBSR_MASK     0x03
#define OHCI_CTRL_PLE           (1 << 2)
#define OHCI_CTRL_IE            (1 << 3)
#define OHCI_CTRL_CLE           (1 << 4)
#define OHCI_CTRL_BLE           (1 << 5)
#define OHCI_CTRL_HCFS_MASK     (3 << 6)
#define OHCI_CTRL_HCFS_RESET    (0 << 6)
#define OHCI_CTRL_HCFS_RESUME   (1 << 6)
#define OHCI_CTRL_HCFS_OPER     (2 << 6)
#define OHCI_CTRL_HCFS_SUSPEND  (3 << 6)

// OHCI Command Status bits
#define OHCI_CMDSTS_HCR         (1 << 0)
#define OHCI_CMDSTS_CLF         (1 << 1)
#define OHCI_CMDSTS_BLF         (1 << 2)

// OHCI Interrupt bits
#define OHCI_INTR_SO            (1 << 0)
#define OHCI_INTR_WDH           (1 << 1)
#define OHCI_INTR_SF            (1 << 2)
#define OHCI_INTR_RD            (1 << 3)
#define OHCI_INTR_UE            (1 << 4)
#define OHCI_INTR_FNO           (1 << 5)
#define OHCI_INTR_RHSC          (1 << 6)
#define OHCI_INTR_MIE           (1 << 31)

// Root Hub Port Status bits
#define OHCI_PORT_CCS           (1 << 0)
#define OHCI_PORT_PES           (1 << 1)
#define OHCI_PORT_PSS           (1 << 2)
#define OHCI_PORT_PRS           (1 << 4)
#define OHCI_PORT_PPS           (1 << 8)
#define OHCI_PORT_LSDA          (1 << 9)
#define OHCI_PORT_CSC           (1 << 16)
#define OHCI_PORT_PESC          (1 << 17)
#define OHCI_PORT_PRSC          (1 << 20)

// ============================================================================
// OHCI HCCA (Host Controller Communication Area) — 256 bytes, 256-byte aligned
// ============================================================================
typedef struct __attribute__((packed, aligned(256))) {
    uint32_t interrupt_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t  reserved[116];
} ohci_hcca_t;

// ============================================================================
// OHCI Endpoint Descriptor (ED) — 16 bytes, 16-byte aligned
// ============================================================================
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t control;
    uint32_t tail_td;
    uint32_t head_td;
    uint32_t next_ed;
} ohci_ed_t;

// ============================================================================
// OHCI Transfer Descriptor (TD) — 16 bytes, 16-byte aligned
// ============================================================================
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t control;
    uint32_t cbp;
    uint32_t next_td;
    uint32_t be;
} ohci_td_t;

// ============================================================================
// Per-controller state
// ============================================================================
typedef struct {
    volatile uint32_t *regs;
    ohci_hcca_t *hcca;
    uint64_t hcca_phys;
    int num_ports;
    int initialized;
    int acpi_validated;       /* 1 if ACPI _CRS confirmed MMIO base */
    uint64_t mmio_phys_addr;  /* Physical MMIO address from PCI BAR */
    usb_hcd_t hcd;
    // Non-blocking keyboard state
    usb_device_t *pending_dev;
    uint8_t *pending_buf;
    ohci_ed_t *pending_ed;
    ohci_td_t *pending_td;
    ohci_td_t *pending_tail;
    uint32_t pending_ed_next; // saved next_ed for removal
} ohci_controller_t;

void init_ohci(pci_device_t *dev);
void poll_ohci_ports(void);
bool is_ohci_ready(void);
void ohci_rescan_ports(void);