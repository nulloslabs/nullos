#pragma once

#include <freestanding/stdint.h>
#include <io/pci.h>
#include <io/usb.h>

// ============================================================================
// EHCI Capability Register Offsets (from BAR0)
// ============================================================================
#define EHCI_CAP_CAPLENGTH      0x00
#define EHCI_CAP_HCIVERSION     0x02
#define EHCI_CAP_HCSPARAMS      0x04
#define EHCI_CAP_HCCPARAMS      0x08

// ============================================================================
// EHCI Operational Register Offsets (from BAR0 + CAPLENGTH)
// ============================================================================
#define EHCI_OP_USBCMD          0x00
#define EHCI_OP_USBSTS          0x04
#define EHCI_OP_USBINTR         0x08
#define EHCI_OP_FRINDEX         0x0C
#define EHCI_OP_CTRLDSSEGMENT   0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR   0x18
#define EHCI_OP_CONFIGFLAG      0x40
#define EHCI_OP_PORTSC_BASE     0x44

// USBCMD bits
#define EHCI_CMD_RS             (1 << 0)
#define EHCI_CMD_HCRESET        (1 << 1)
#define EHCI_CMD_PSE            (1 << 4)
#define EHCI_CMD_ASE            (1 << 5)
#define EHCI_CMD_IAAD           (1 << 6)
#define EHCI_CMD_ITC_MASK       (0xFF << 16)

// USBSTS bits
#define EHCI_STS_USBINT         (1 << 0)
#define EHCI_STS_USBERRINT      (1 << 1)
#define EHCI_STS_PCD            (1 << 2)    // Port Change Detect
#define EHCI_STS_FLR            (1 << 3)
#define EHCI_STS_HSE            (1 << 4)
#define EHCI_STS_IAA            (1 << 5)
#define EHCI_STS_HALTED         (1 << 12)
#define EHCI_STS_RECLAMATION    (1 << 13)
#define EHCI_STS_PSS            (1 << 14)
#define EHCI_STS_ASS            (1 << 15)

// PORTSC bits
#define EHCI_PORT_CCS           (1 << 0)
#define EHCI_PORT_CSC           (1 << 1)
#define EHCI_PORT_PED           (1 << 2)
#define EHCI_PORT_PEDC          (1 << 3)
#define EHCI_PORT_OCA           (1 << 4)
#define EHCI_PORT_OCC           (1 << 5)
#define EHCI_PORT_FPR           (1 << 6)
#define EHCI_PORT_SUSPEND       (1 << 7)
#define EHCI_PORT_RESET         (1 << 8)
#define EHCI_PORT_LINE_STATUS   (3 << 10)
#define EHCI_PORT_PP            (1 << 12)
#define EHCI_PORT_OWNER         (1 << 13)

// CONFIGFLAG
#define EHCI_CF_FLAG            (1 << 0)

// HCSPARAMS field masks
#define EHCI_HCS_N_PORTS_MASK   0x0F
#define EHCI_HCS_N_PCC_SHIFT    8
#define EHCI_HCS_N_PCC_MASK     (0x0F << 8)   // Ports Per Companion Controller
#define EHCI_HCS_N_CC_SHIFT     12
#define EHCI_HCS_N_CC_MASK      (0x0F << 12)  // Number of Companion Controllers

// HCCPARAMS
#define EHCI_HCC_64BIT          (1 << 0)
#define EHCI_HCC_EECP_MASK     (0xFF << 8)
#define EHCI_HCC_EECP_SHIFT    8

// ============================================================================
// EHCI Queue Head (QH) — 48+ bytes, 32-byte aligned
// ============================================================================
typedef struct __attribute__((packed, aligned(32))) {
    uint32_t next_qh;
    uint32_t characteristics;
    uint32_t capabilities;
    uint32_t current_qtd;
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qh_t;

// ============================================================================
// EHCI Queue Element Transfer Descriptor (qTD) — 32 bytes, 32-byte aligned
// ============================================================================
typedef struct __attribute__((packed, aligned(32))) {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qtd_t;

// QH/qTD link pointer types
#define EHCI_PTR_TERMINATE      (1 << 0)
#define EHCI_PTR_TYPE_ITD       (0 << 1)
#define EHCI_PTR_TYPE_QH        (1 << 1)
#define EHCI_PTR_TYPE_SITD      (2 << 1)
#define EHCI_PTR_TYPE_FSTN      (3 << 1)

// qTD token bits
#define EHCI_QTD_ACTIVE         (1 << 7)
#define EHCI_QTD_PID_OUT        (0 << 8)
#define EHCI_QTD_PID_IN         (1 << 8)
#define EHCI_QTD_PID_SETUP      (2 << 8)
#define EHCI_QTD_IOC            (1 << 15)  // Interrupt on Complete

// ============================================================================
// Per-controller state
// ============================================================================
typedef struct {
    volatile uint8_t *cap_regs;
    volatile uint8_t *op_regs;
    uint32_t *periodic_list;
    uint64_t periodic_list_phys;
    ehci_qh_t *async_qh;
    ehci_qh_t *intr_qh;
    ehci_qtd_t *intr_qtd;
    int intr_periodic_linked;
    int num_ports;
    int n_pcc;              // Ports per companion controller (from HCSPARAMS)
    int initialized;
    int acpi_validated;     /* 1 if ACPI _CRS confirmed MMIO base */
    uint64_t mmio_phys_addr;
    usb_hcd_t hcd;
    usb_device_t *pending_dev;  // Device with outstanding interrupt transfer (NULL if none)
    int pending_kbd_idx;        // kbd_list index for the pending device (-1 if none)
} ehci_controller_t;

void init_ehci(pci_device_t *dev);
void poll_ehci_ports(void);