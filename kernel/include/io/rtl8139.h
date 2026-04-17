#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>
#include <io/pci.h>

// --- PCI ID ---
#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

// --- Registers (IO offset) ---
#define RTL_MAC0 0x00 // MAC address bytes 0-3
#define RTL_MAC4 0x04 // MAC address bytes 4-5
#define RTL_MAR0 0x08 // Multicast filter
#define RTL_TSAD0 0x20 // TX start address, slot 0
#define RTL_TSAD1 0x24 // TX start address, slot 1
#define RTL_TSAD2 0x28 // TX start address, slot 2
#define RTL_TSAD3 0x2C // TX start address, slot 3
#define RTL_RBSTART 0x30  // RX ring buffer start (physical addr)
#define RTL_CMD 0x37 // Command register
#define RTL_CAPR 0x38 // Current address of packet read
#define RTL_CBR 0x3A // Current buffer address (RX)
#define RTL_IMR 0x3C // Interrupt mask register
#define RTL_ISR 0x3E // Interrupt status register
#define RTL_TCR 0x40 // TX config register
#define RTL_RCR 0x44 // RX config register
#define RTL_MPC 0x4C // Missed packet counter
#define RTL_CFG9346 0x52 // 93C46 command register (unlock/lock)
#define RTL_CONFIG1 0x52 // Config register 1
#define RTL_TSD0 0x10 // TX status, slot 0
#define RTL_TSD1 0x14 // TX status, slot 1
#define RTL_TSD2 0x18 // TX status, slot 2
#define RTL_TSD3 0x1C // TX status, slot 3

// --- CMD bits ---
#define RTL_CMD_RST (1 << 4) // Software reset
#define RTL_CMD_RE (1 << 3) // RX enable
#define RTL_CMD_TE (1 << 2) // TX enable
#define RTL_CMD_BUFE (1 << 0) // RX buffer empty

// --- ISR/IMR bits ---
#define RTL_INT_ROK (1 << 0) // RX OK
#define RTL_INT_RER (1 << 1) // RX error
#define RTL_INT_TOK (1 << 2) // TX OK
#define RTL_INT_TER (1 << 3) // TX error
#define RTL_INT_RXOVW (1 << 4) // RX buffer overflow

// --- RCR bits ---
#define RTL_RCR_AAP (1 << 0) // Accept all packets (promiscuous)
#define RTL_RCR_APM (1 << 1) // Accept physical match
#define RTL_RCR_AM (1 << 2) // Accept multicast
#define RTL_RCR_AB (1 << 3) // Accept broadcast
#define RTL_RCR_WRAP (1 << 7) // Wrap RX buffer
#define RTL_RCR_RBLEN_64K (3 << 11) // 64K RX buffer

// --- TSD bits ---
#define RTL_TSD_OWN (1 << 13) // DMA complete (we own the descriptor)
#define RTL_TSD_TOK (1 << 15) // TX OK

// --- Buffer sizes ---
#define RTL_RX_BUF_SIZE (64 * 1024 + 16 + 1500) // 64K + wrap padding
#define RTL_TX_BUF_SIZE 1536 // max ethernet frame
#define RTL_TX_SLOTS 4

// --- Ethernet frame ---
typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
    uint8_t payload[];
} __attribute__((packed)) eth_frame_t;

// --- Driver state ---
typedef struct {
    uint16_t io_base;
    uint8_t mac[6];
    uint8_t rx_buf[RTL_RX_BUF_SIZE] __attribute__((aligned(4)));
    uint8_t tx_buf[RTL_TX_SLOTS][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
    uint16_t rx_offset;  // current read offset in RX ring
    int tx_slot;    // next TX slot (0-3, round robin)
    bool ready;
} rtl8139_t;

extern rtl8139_t rtl8139;

void init_rtl8139(pci_device_t *dev);
bool rtl8139_send(const void *data, uint16_t len);
void rtl8139_poll(void);  // call in main loop if not using interrupts
void rtl8139_get_mac(uint8_t mac[6]);

typedef void (*rtl8139_rx_callback_t)(const uint8_t *data, uint16_t len);