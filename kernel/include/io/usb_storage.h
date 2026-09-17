#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <io/usb.h>

#define USB_MSC_CLASS              0x08
#define USB_MSC_SUBCLASS_SCSI      0x06
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50

#define USB_STORAGE_CBW_SIGNATURE 0x43425355
#define USB_STORAGE_CSW_SIGNATURE 0x53425355
#define USB_STORAGE_CBW_LENGTH    31
#define USB_STORAGE_CSW_LENGTH    13

#define USB_STORAGE_GET_MAX_LUN 0xFE
#define USB_STORAGE_MASS_RESET  0xFF

#define USB_STORAGE_STATUS_PASS  0x00
#define USB_STORAGE_STATUS_FAIL  0x01
#define USB_STORAGE_STATUS_PHASE 0x02

#define USB_SCSI_TEST_UNIT_READY 0x00
#define USB_SCSI_REQUEST_SENSE   0x03
#define USB_SCSI_INQUIRY         0x12
#define USB_SCSI_READ_CAPACITY10 0x25

#define USB_SCSI_READ10     0x28
#define USB_SCSI_WRITE10    0x2A
#define USB_SCSI_START_STOP 0x1B

#define USB_STORAGE_MAX_DEVICES 8
#define USB_STORAGE_MAX_LUN     15
#define USB_STORAGE_BULK_CHUNK  2048

#define USB_STORAGE_MAX_BLOCKS_PER_CMD 32
#define USB_STORAGE_TUR_RETRIES        10
#define USB_STORAGE_CSW_RETRIES        3

#define USB_STORAGE_SENSE_LENGTH    18
#define USB_STORAGE_INQUIRY_LENGTH  36
#define USB_STORAGE_CAPACITY_LENGTH 8

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
} __attribute__((packed)) usb_storage_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) usb_storage_csw_t;

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
} usb_storage_entry_t;

extern usb_storage_entry_t storage_entries[USB_STORAGE_MAX_DEVICES];

int get_usb_storage_count(void);
uint64_t get_usb_storage_size(int index);
uint32_t get_usb_storage_block_size(int index);
bool make_usb_storage_name(char *name, uint64_t size, int index);
uint64_t read_usb_storage_device(void *buf, uint64_t count, uint64_t offset, int index, void *handle);
uint64_t write_usb_storage_device(const void *buf, uint64_t count, uint64_t offset, int index, void *handle);
void remove_usb_storage(usb_hcd_t *hcd, uint8_t port_id);
void init_usb_storage(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id);
