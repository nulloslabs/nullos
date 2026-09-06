#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <io/usb.h>

#define USB_MSC_CLASS              0x08
#define USB_MSC_SUBCLASS_SCSI      0x06
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50

#define USB_BOT_CBW_SIGNATURE 0x43425355
#define USB_BOT_CSW_SIGNATURE 0x53425355
#define USB_BOT_CBW_LENGTH    31
#define USB_BOT_CSW_LENGTH    13

#define USB_BOT_GET_MAX_LUN 0xFE
#define USB_BOT_MASS_RESET  0xFF

#define USB_BOT_STATUS_PASS  0x00
#define USB_BOT_STATUS_FAIL  0x01
#define USB_BOT_STATUS_PHASE 0x02

#define USB_SCSI_TEST_UNIT_READY 0x00
#define USB_SCSI_REQUEST_SENSE   0x03
#define USB_SCSI_INQUIRY         0x12
#define USB_SCSI_READ_CAPACITY10 0x25

#define USB_SCSI_READ10     0x28
#define USB_SCSI_WRITE10    0x2A
#define USB_SCSI_START_STOP 0x1B

#define USB_BOT_MAX_DEVICES 8
#define USB_BOT_MAX_LUN     15
#define USB_BOT_BULK_CHUNK  2048

#define USB_BOT_MAX_BLOCKS_PER_CMD 32
#define USB_BOT_TUR_RETRIES        10
#define USB_BOT_CSW_RETRIES        3

#define USB_BOT_SENSE_LENGTH    18
#define USB_BOT_INQUIRY_LENGTH  36
#define USB_BOT_CAPACITY_LENGTH 8

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
} __attribute__((packed)) usb_bot_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) usb_bot_csw_t;

typedef struct {
    usb_device_t *dev;
    usb_hcd_t *hcd;
    uint8_t interface_number;
    uint8_t lun;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t total_size;
    char name[16];
    bool present;
} usb_bot_entry_t;

extern usb_bot_entry_t bot_entries[USB_BOT_MAX_DEVICES];

int get_usb_bot_count(void);
uint64_t get_usb_bot_size(int index);
uint32_t get_usb_bot_block_size(int index);
bool make_usb_bot_name(char *name, uint64_t size, int index);
uint64_t read_usb_bot_device(void *buf, uint64_t count, uint64_t offset, int index);
uint64_t write_usb_bot_device(const void *buf, uint64_t count, uint64_t offset, int index);
void remove_usb_bot(usb_hcd_t *hcd, uint8_t port_id);
void init_usb_bot(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id);
