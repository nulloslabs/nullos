#pragma once

#include <stdint.h>
#include <io/pci.h>
#include <io/usb.h>

#define MAX_OHCI_CONTROLLERS 8
#define OHCI_MAX_PORTS 15

#define OHCI_REVISION          0x00
#define OHCI_CONTROL           0x04
#define OHCI_COMMAND_STATUS    0x08
#define OHCI_INTERRUPT_STATUS  0x0C
#define OHCI_INTERRUPT_ENABLE  0x10
#define OHCI_INTERRUPT_DISABLE 0x14
#define OHCI_HCCA              0x18
#define OHCI_PERIOD_CURRENT_ED 0x1C
#define OHCI_CONTROL_HEAD_ED   0x20
#define OHCI_CONTROL_CURRENT_ED 0x24
#define OHCI_BULK_HEAD_ED      0x28
#define OHCI_BULK_CURRENT_ED   0x2C
#define OHCI_DONE_HEAD         0x30
#define OHCI_FRAME_INTERVAL    0x34
#define OHCI_FRAME_REMAINING   0x38
#define OHCI_FRAME_NUMBER      0x3C
#define OHCI_PERIODIC_START    0x40
#define OHCI_LOW_SPEED_THRESHOLD 0x44
#define OHCI_RH_DESCRIPTOR_A   0x48
#define OHCI_RH_DESCRIPTOR_B   0x4C
#define OHCI_RH_STATUS         0x50
#define OHCI_RH_PORT_STATUS(n) (0x54 + (n) * 4)

#define OHCI_CONTROL_PLE       (1u << 2)
#define OHCI_CONTROL_CLE       (1u << 4)
#define OHCI_CONTROL_HCFS      (3u << 6)
#define OHCI_CONTROL_IR        (1u << 8)
#define OHCI_CONTROL_RWC       (1u << 9)
#define OHCI_USB_RESET         (0u << 6)
#define OHCI_USB_RESUME        (1u << 6)
#define OHCI_USB_OPERATIONAL   (2u << 6)
#define OHCI_USB_SUSPEND       (3u << 6)

#define OHCI_COMMAND_HCR       (1u << 0)
#define OHCI_COMMAND_CLF       (1u << 1)
#define OHCI_COMMAND_OCR       (1u << 3)

#define OHCI_INTERRUPT_WDH     (1u << 1)
#define OHCI_INTERRUPT_UE      (1u << 4)
#define OHCI_INTERRUPT_RHSC    (1u << 6)
#define OHCI_INTERRUPT_OC      (1u << 30)
#define OHCI_INTERRUPT_MIE     (1u << 31)

#define OHCI_RH_A_NDP          0x000000FFu
#define OHCI_RH_A_PSM          (1u << 8)
#define OHCI_RH_A_NPS          (1u << 9)
#define OHCI_RH_A_POTPGT       0xFF000000u
#define OHCI_RH_STATUS_LPSC    (1u << 16)

#define OHCI_PORT_CCS          0x00000001u
#define OHCI_PORT_PES          0x00000002u
#define OHCI_PORT_PRS          0x00000010u
#define OHCI_PORT_PPS          0x00000100u
#define OHCI_PORT_LSDA         0x00000200u
#define OHCI_PORT_CSC          0x00010000u
#define OHCI_PORT_PESC         0x00020000u
#define OHCI_PORT_PSSC         0x00040000u
#define OHCI_PORT_OCIC         0x00080000u
#define OHCI_PORT_PRSC         0x00100000u
#define OHCI_PORT_CHANGE_MASK  (OHCI_PORT_CSC | OHCI_PORT_PESC | \
                                OHCI_PORT_PSSC | OHCI_PORT_OCIC | \
                                OHCI_PORT_PRSC)

#define OHCI_ED_SKIP           (1u << 14)
#define OHCI_ED_LOW_SPEED      (1u << 13)
#define OHCI_ED_OUT            (1u << 11)
#define OHCI_ED_IN             (2u << 11)
#define OHCI_ED_TOGGLE_CARRY   0x00000002u
#define OHCI_ED_HALTED         0x00000001u

#define OHCI_TD_CC             0xF0000000u
#define OHCI_TD_NOT_ACCESSED   0xF0000000u
#define OHCI_TD_DATA0          0x02000000u
#define OHCI_TD_DATA1          0x03000000u
#define OHCI_TD_DELAY(n)       (((uint32_t)(n) & 7u) << 21)
#define OHCI_TD_SETUP          0x00000000u
#define OHCI_TD_OUT            0x00080000u
#define OHCI_TD_IN             0x00100000u
#define OHCI_TD_ROUND          0x00040000u

#define OHCI_FRAME_INTERVAL_VALUE 0x2EDFu
#define OHCI_LOW_SPEED_THRESHOLD_VALUE 0x0628u

typedef struct {
    volatile uint32_t interrupt_table[32];
    volatile uint32_t frame_number;
    volatile uint32_t done_head;
    uint8_t reserved[120];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

typedef struct {
    volatile uint32_t flags;
    volatile uint32_t tail_pointer;
    volatile uint32_t head_pointer;
    volatile uint32_t next_ed;
} __attribute__((packed, aligned(16))) ohci_ed_t;

typedef struct {
    volatile uint32_t flags;
    volatile uint32_t current_buffer;
    volatile uint32_t next_td;
    volatile uint32_t buffer_end;
} __attribute__((packed, aligned(16))) ohci_td_t;

typedef struct {
    volatile uint8_t *registers;
    void *mmio_mapping;
    uint64_t mmio_phys;
    uint8_t mmio_pages;
    ohci_hcca_t *hcca;
    uint64_t hcca_phys;
    uint8_t *control_page;
    uint64_t control_page_phys;
    ohci_ed_t *control_ed;
    uint8_t *interrupt_page;
    uint64_t interrupt_page_phys;
    ohci_ed_t *interrupt_ed;
    ohci_td_t *interrupt_td;
    ohci_td_t *interrupt_dummy_td;
    uint8_t *interrupt_buffer;
    usb_hcd_t hcd;
    usb_device_t *pending_dev;
    uint8_t *pending_buffer;
    uint16_t pending_length;
    uint16_t present_ports;
    int keyboard_cursor;
    uint8_t num_ports;
    bool control_busy;
    bool initialized;
} ohci_controller_t;

bool is_ohci_ready(void);
void poll_ohci_ports(void);
void init_ohci(pci_device_t *dev);
