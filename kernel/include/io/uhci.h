#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <io/pci.h>
#include <io/usb.h>

#define MAX_UHCI_CONTROLLERS 8

#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRNUM     0x06
#define UHCI_FLBASEADD 0x08
#define UHCI_SOFMOD    0x0C
#define UHCI_PORTSC1   0x10
#define UHCI_PORTSC2   0x12

#define UHCI_CMD_RS      (1 << 0)
#define UHCI_CMD_HCRESET (1 << 1)
#define UHCI_CMD_GRESET  (1 << 2)
#define UHCI_CMD_EGRE    (1 << 3)
#define UHCI_CMD_FGR     (1 << 4)
#define UHCI_CMD_SWDBG   (1 << 5)
#define UHCI_CMD_CF      (1 << 6)
#define UHCI_CMD_MAXP    (1 << 7)

#define UHCI_STS_USBINT    (1 << 0)
#define UHCI_STS_USBERRINT (1 << 1)
#define UHCI_STS_RESUME    (1 << 2)
#define UHCI_STS_HCHALTED  (1 << 3)
#define UHCI_STS_HCPROCESS (1 << 4)
#define UHCI_STS_HCSYSERR  (1 << 5)

#define UHCI_PORT_CCS   (1 << 0)
#define UHCI_PORT_CSC   (1 << 1)
#define UHCI_PORT_PED   (1 << 2)
#define UHCI_PORT_PEDC  (1 << 3)
#define UHCI_PORT_LSDA  (1 << 8)
#define UHCI_PORT_RESET (1 << 9)
#define UHCI_PORT_SUSP  (1 << 12)

#define UHCI_PTR_TERMINATE (1 << 0)
#define UHCI_PTR_QH        (1 << 1)
#define UHCI_PTR_DEPTH     (1 << 2)

#define UHCI_TD_ACTIVE (1 << 23)
#define UHCI_TD_LS     (1 << 26)
#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1

#define UHCI_TD_ACTLEN_MASK 0x7FF
#define UHCI_TD_STATUS_ACTIVE (1 << 23)

#define UHCI_FRAME_LIST_SIZE 1024

typedef struct {
    uint32_t head_link_ptr;
    uint32_t element_link_ptr;
} __attribute__((packed)) uhci_qh_t;

typedef struct {
    uint32_t link_ptr;
    uint32_t status;
    uint32_t token;
    uint32_t buffer_ptr;
} __attribute__((packed)) uhci_td_t;

typedef struct {
    uint16_t io_base;
    uint32_t *frame_list;
    uint64_t frame_list_phys;
    uhci_qh_t *qh;
    uhci_qh_t *intr_qh;
    usb_hcd_t hcd;
    int initialized;
    // ACPI validation
    int acpi_validated;       /* 1 if ACPI _CRS confirmed io_base */
    uint16_t num_ports;       /* Port count from ACPI (0 = use default 2) */
    // Non-blocking keyboard state
    usb_device_t *pending_dev;
    uint8_t *pending_buf;
    uhci_td_t *pending_td;
    uint16_t pending_len;
} uhci_controller_t;

void init_uhci(pci_device_t *dev);
void poll_uhci_ports(void);
bool is_uhci_ready(void);
void rescan_uhci_ports(int ctrl_idx);