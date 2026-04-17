#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <io/pci.h>

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E

// Register Offsets
#define E1000_CTRL     0x0000
#define E1000_STATUS   0x0008
#define E1000_EEPROM   0x0014
#define E1000_CTRL_EXT 0x0018
#define E1000_ICR      0x00C0
#define E1000_IMS      0x00D0
#define E1000_RCTL     0x0100
#define E1000_TCTL     0x0400
#define E1000_RDBAL    0x2800
#define E1000_RDBAH    0x2804
#define E1000_RDLEN    0x2808
#define E1000_RDH      0x2810
#define E1000_RDT      0x2818
#define E1000_TDBAL    0x3800
#define E1000_TDBAH    0x3804
#define E1000_TDLEN    0x3808
#define E1000_TDH      0x3810
#define E1000_TDT      0x3818
#define E1000_MTA      0x5200
#define E1000_RAL      0x5400
#define E1000_RAH      0x5404

// RCTL bits
#define RCTL_EN             (1 << 1)    // Receiver Enable
#define RCTL_SBP            (1 << 2)    // Store Bad Packets
#define RCTL_UPE            (1 << 3)    // Unicast Promiscuous Enabled
#define RCTL_MPE            (1 << 4)    // Multicast Promiscuous Enabled
#define RCTL_LPE            (1 << 5)    // Long Packet Reception Enable
#define RCTL_BAM            (1 << 15)   // Broadcast Accept Mode
#define RCTL_BSIZE_2048     (0 << 16)
#define RCTL_SECRC          (1 << 26)   // Strip Ethernet CRC

// TCTL bits
#define TCTL_EN             (1 << 1)    // Transmit Enable
#define TCTL_PSP            (1 << 3)    // Pad Short Packets

// Descriptors
#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

#define CMD_EOP  (1 << 0)
#define CMD_IFCS (1 << 1)
#define CMD_RS   (1 << 3)

void init_e1000(pci_device_t *dev);
bool e1000_send(const void *data, uint16_t len);
void e1000_get_mac(uint8_t mac[6]);
