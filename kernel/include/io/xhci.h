#pragma once

#include <freestanding/stdint.h>
#include <io/pci.h>
#include <io/usb.h>

// ============================================================================
// xHCI Capability Register Offsets (from BAR0)
// ============================================================================
#define XHCI_CAP_CAPLENGTH      0x00
#define XHCI_CAP_HCIVERSION     0x02
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCSPARAMS3     0x0C
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18
#define XHCI_CAP_HCCPARAMS2     0x1C

// ============================================================================
// xHCI Operational Register Offsets (from BAR0 + CAPLENGTH)
// ============================================================================
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR_LO         0x18
#define XHCI_OP_CRCR_HI         0x1C
#define XHCI_OP_DCBAAP_LO       0x30
#define XHCI_OP_DCBAAP_HI       0x34
#define XHCI_OP_CONFIG          0x38
#define XHCI_OP_PORTSC_BASE     0x400

// USBCMD bits
#define XHCI_CMD_RS             (1 << 0)
#define XHCI_CMD_HCRESET        (1 << 1)
#define XHCI_CMD_INTE           (1 << 2)
#define XHCI_CMD_HSEE           (1 << 3)

// USBSTS bits
#define XHCI_STS_HCH            (1 << 0)
#define XHCI_STS_EINT           (1 << 3)    // Event Interrupt
#define XHCI_STS_PCD            (1 << 4)    // Port Change Detect
#define XHCI_STS_CNR            (1 << 11)

// PORTSC bits
#define XHCI_PORT_CCS           (1 << 0)
#define XHCI_PORT_PED           (1 << 1)
#define XHCI_PORT_OCA           (1 << 3)
#define XHCI_PORT_PR            (1 << 4)
#define XHCI_PORT_PLS_MASK      (0xF << 5)
#define XHCI_PORT_PLS_U0        (0 << 5)
#define XHCI_PORT_PLS_RXDETECT  (5 << 5)
#define XHCI_PORT_PP            (1 << 9)
#define XHCI_PORT_SPEED_MASK    (0xF << 10)
#define XHCI_PORT_SPEED_SHIFT   10
#define XHCI_PORT_CSC           (1 << 17)
#define XHCI_PORT_PEC           (1 << 18)
#define XHCI_PORT_WRC           (1 << 19)
#define XHCI_PORT_OCC           (1 << 20)
#define XHCI_PORT_PRC           (1 << 21)
#define XHCI_PORT_PLC           (1 << 22)
#define XHCI_PORT_CEC           (1 << 23)
// Preserve mask: read-write bits that should NOT be cleared during W1C acks.
// CCS(bit0) and PED(bit1) control port enable — must be preserved on writes.
// Also preserve PP(bit9), OCA(bit3), and all RW1C bits are NOT preserved here.
#define XHCI_PORT_RW_MASK       0x0E018861

// Port speeds
#define XHCI_SPEED_FULL         1
#define XHCI_SPEED_LOW          2
#define XHCI_SPEED_HIGH         3
#define XHCI_SPEED_SUPER        4

// ============================================================================
// xHCI Runtime Register Offsets (from BAR0 + RTSOFF)
// ============================================================================
#define XHCI_RT_IMAN(n)         (0x20 + (32 * (n)))
#define XHCI_RT_IMOD(n)         (0x24 + (32 * (n)))
#define XHCI_RT_ERSTSZ(n)       (0x28 + (32 * (n)))
#define XHCI_RT_ERSTBA_LO(n)    (0x30 + (32 * (n)))
#define XHCI_RT_ERSTBA_HI(n)    (0x34 + (32 * (n)))
#define XHCI_RT_ERDP_LO(n)      (0x38 + (32 * (n)))
#define XHCI_RT_ERDP_HI(n)      (0x3C + (32 * (n)))

// ============================================================================
// TRB (Transfer Request Block) — 16 bytes
// ============================================================================
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

// TRB types (control field bits 15:10)
#define XHCI_TRB_TYPE_SHIFT     10
#define XHCI_TRB_NORMAL         (1 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_SETUP          (2 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_DATA           (3 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_STATUS         (4 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_LINK           (6 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_ENABLE_SLOT    (9 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_DISABLE_SLOT   (10 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_ADDRESS_DEV    (11 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_CONFIG_EP      (12 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_NOOP           (23 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_CMD_COMPLETION (33 << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_PORT_CHANGE    (34 << XHCI_TRB_TYPE_SHIFT)

// TRB control bits
#define XHCI_TRB_CYCLE          (1 << 0)
#define XHCI_TRB_IOC            (1 << 5)

// ============================================================================
// Event Ring Segment Table Entry
// ============================================================================
typedef struct __attribute__((packed, aligned(64))) {
    uint64_t ring_base;
    uint32_t ring_size;
    uint32_t reserved;
} xhci_erst_entry_t;

// Ring sizes
#define XHCI_CMD_RING_SIZE      256
#define XHCI_EVT_RING_SIZE      256

// ============================================================================
// Per-controller state
// ============================================================================
typedef struct {
    volatile uint8_t *cap_regs;
    volatile uint8_t *op_regs;
    volatile uint8_t *rt_regs;
    volatile uint32_t *db_regs;

    uint64_t *dcbaap;
    uint64_t dcbaap_phys;

    xhci_trb_t *cmd_ring;
    uint64_t cmd_ring_phys;
    int cmd_ring_enqueue;
    int cmd_ring_cycle;

    xhci_trb_t *evt_ring;
    uint64_t evt_ring_phys;
    xhci_erst_entry_t *erst;
    uint64_t erst_phys;
    int evt_ring_dequeue;
    int evt_ring_cycle;

    void *dev_ctx[256];
    xhci_trb_t *ep1_rings[256];
    int ep1_enqueue[256];
    int ep1_cycle[256];

    int ctx_stride;
    int max_slots;
    int max_ports;
    int initialized;
    int acpi_validated;     /* 1 if ACPI _CRS confirmed MMIO base */
    uint64_t mmio_phys_addr;
    usb_hcd_t hcd;
} xhci_controller_t;

// xHCI Contexts
typedef struct __attribute__((packed)) {
    uint32_t drop_context_flags;
    uint32_t add_context_flags;
    uint32_t reserved[6];
} xhci_input_ctrl_ctx_t;

typedef struct __attribute__((packed)) {
    uint32_t info1; // Route string, speed, mtt, hub
    uint32_t info2; // Max exit latency, root hub port number, number of ports
    uint32_t info3; // TT hub slot id, TT port num, TTT, Ufo, Interrupter target
    uint32_t info4; // Device state
    uint32_t reserved[4];
} xhci_slot_ctx_t;

typedef struct __attribute__((packed)) {
    uint32_t info1; // State, mult, max pstreams, interval, max esit hz
    uint32_t info2; // error count, ep type, host init ds, max burst sz, max packet size
    uint64_t tr_dequeue_ptr; // TR dequeue ptr, DCS
    uint32_t tx_info; // Avg trb len, max esit payload
    uint32_t reserved[3];
} xhci_ep_ctx_t;

typedef struct __attribute__((packed, aligned(64))) {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t eps[31];
} xhci_device_ctx_t;

typedef struct __attribute__((packed, aligned(64))) {
    xhci_input_ctrl_ctx_t ictrl;
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t eps[31];
} xhci_input_ctx_t;

void init_xhci(pci_device_t *dev);
void poll_xhci_ports(void);
