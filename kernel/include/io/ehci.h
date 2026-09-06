#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <io/pci.h>
#include <io/usb.h>

#define MAX_EHCI_CONTROLLERS 8
#define EHCI_MAX_PORTS       15

#define EHCI_CAP_CAPLENGTH 0x00
#define EHCI_CAP_HCSPARAMS 0x04
#define EHCI_CAP_HCCPARAMS 0x08

#define EHCI_OP_USBCMD           0x00
#define EHCI_OP_USBSTS           0x04
#define EHCI_OP_USBINTR          0x08
#define EHCI_OP_FRINDEX          0x0C
#define EHCI_OP_CTRLDSSEGMENT    0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR    0x18
#define EHCI_OP_CONFIGFLAG       0x40
#define EHCI_OP_PORTSC(n)        (0x44 + (n) * 4)

#define EHCI_CMD_RUN     (1u << 0)
#define EHCI_CMD_HCRESET (1u << 1)
#define EHCI_CMD_PSE     (1u << 4)
#define EHCI_CMD_ASE     (1u << 5)
#define EHCI_CMD_IAAD    (1u << 6)
#define EHCI_CMD_ITC(n)  ((uint32_t)(n) << 16)

#define EHCI_STS_USBINT    (1u << 0)
#define EHCI_STS_PCD       (1u << 2)
#define EHCI_STS_IAA       (1u << 5)
#define EHCI_STS_HCHALTED  (1u << 12)
#define EHCI_STS_PSS       (1u << 14)
#define EHCI_STS_ASS       (1u << 15)

#define EHCI_HCS_N_PORTS 0x0000000Fu
#define EHCI_HCS_PPC     (1u << 4)
#define EHCI_HCS_POTPGT  0x00F00000u

#define EHCI_HCC_ECP 0x0000FF00u

#define EHCI_LEGSUP_BIOS (1u << 16)
#define EHCI_LEGSUP_OS   (1u << 24)

#define EHCI_FLAG_CF 1u

#define EHCI_PORT_CCS     (1u << 0)
#define EHCI_PORT_CSC     (1u << 1)
#define EHCI_PORT_PED     (1u << 2)
#define EHCI_PORT_PEC     (1u << 3)
#define EHCI_PORT_OCC     (1u << 5)
#define EHCI_PORT_FPR     (1u << 6)
#define EHCI_PORT_SUSPEND (1u << 7)
#define EHCI_PORT_PR      (1u << 8)
#define EHCI_PORT_LINE    (3u << 10)
#define EHCI_PORT_LINE_FS (1u << 10)
#define EHCI_PORT_LINE_LS (2u << 10)
#define EHCI_PORT_PP      (1u << 12)
#define EHCI_PORT_OWNER   (1u << 13)
#define EHCI_PORT_PIC     (3u << 14)
#define EHCI_PORT_CHANGES (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)

#define EHCI_QH_TYPE_QH   (1u << 1)
#define EHCI_QH_ADDR_MASK 0x0000007Fu
#define EHCI_QH_EP_SHIFT  8
#define EHCI_QH_HEAD      (1u << 15)
#define EHCI_QH_SPEED_SHIFT 12
#define EHCI_QH_MAX_PACKET_SHIFT 16
#define EHCI_QH_MULT_ONE  (1u << 30)
#define EHCI_QH_SPEED_LOW (1u << 12)
#define EHCI_QH_SPEED_HIGH (2u << 12)
#define EHCI_QH_SMASK_ALL 0xFFu

#define EHCI_QTD_T        1u
#define EHCI_QTD_ADDR_MASK 0xFFFFFFE0u
#define EHCI_QTD_STATUS   0x000000FFu
#define EHCI_QTD_ACTIVE   (1u << 7)
#define EHCI_QTD_HALTED   (1u << 6)
#define EHCI_QTD_BUFERR   (1u << 5)
#define EHCI_QTD_BABBLE   (1u << 4)
#define EHCI_QTD_XACTERR  (1u << 3)
#define EHCI_QTD_PID_SHIFT 8
#define EHCI_QTD_PID_OUT   0u
#define EHCI_QTD_PID_IN    1u
#define EHCI_QTD_PID_SETUP 2u
#define EHCI_QTD_BYTES    (0x7FFFu << 16)
#define EHCI_QTD_TOGGLE   (1u << 31)
#define EHCI_QTD_BUF_BASE 0xFFFFF000u
#define EHCI_QTD_BUF_OFFSET 0x00000FFFu

#define EHCI_CONTROL_QH_OFFSET 0
#define EHCI_BULK_QH_OFFSET    64
#define EHCI_XFER_QTD_OFFSET   128
#define EHCI_XFER_SETUP_OFFSET 256
#define EHCI_XFER_DATA_OFFSET  512
#define EHCI_XFER_QTD_COUNT    3

#define EHCI_INTERRUPT_QH_OFFSET   2560
#define EHCI_INTERRUPT_QTD_OFFSET  2624
#define EHCI_INTERRUPT_DATA_OFFSET 2656

#define EHCI_FRAME_LIST_ENTRIES 1024
#define EHCI_MAX_CONTROL_DATA   512
#define EHCI_MAX_BULK_DATA      2048
#define EHCI_MAX_INTERRUPT_DATA 64
#define EHCI_QTD_MAX_PAGES      5

#define EHCI_CONTROL_TIMEOUT_MS 1000
#define EHCI_BULK_TIMEOUT_MS    2000
#define EHCI_PSS_TIMEOUT_MS     100
#define EHCI_IAA_TIMEOUT_MS     20
#define EHCI_CMD_RESET_TIMEOUT_MS  250
#define EHCI_RUN_TIMEOUT_MS        100
#define EHCI_PORT_RESET_HOLD_MS    50
#define EHCI_PORT_RESET_HANDSHAKE_US 1000

typedef struct {
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
} __attribute__((packed, aligned(32))) ehci_qtd_t;

typedef struct {
    volatile uint32_t horizontal_link;
    volatile uint32_t characteristics;
    volatile uint32_t capabilities;
    volatile uint32_t current_qtd;
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
} __attribute__((packed, aligned(32))) ehci_qh_t;

typedef struct {
    volatile uint8_t *cap_registers;
    volatile uint8_t *op_registers;
    void *mmio_mapping;
    uint64_t mmio_phys;
    uint8_t mmio_pages;
    uint8_t *dma_page;
    uint64_t dma_page_phys;
    uint8_t *frame_list;
    uint64_t frame_list_phys;
    ehci_qh_t *control_qh;
    ehci_qh_t *bulk_qh;
    uint64_t control_qh_phys;
    uint64_t bulk_qh_phys;
    ehci_qh_t *interrupt_qh;
    ehci_qtd_t *interrupt_qtd;
    uint64_t interrupt_qh_phys;
    uint64_t interrupt_qtd_phys;
    bool control_busy;
    bool bulk_busy;
    usb_device_t *interrupt_dev;
    void *interrupt_buf;
    uint16_t interrupt_len;
    bool interrupt_busy;
    int keyboard_cursor;
    usb_hcd_t hcd;
    uint16_t present_ports;
    uint8_t num_ports;
    bool initialized;
} ehci_controller_t;

void poll_ehci_ports(void);
void init_ehci(pci_device_t *dev);
