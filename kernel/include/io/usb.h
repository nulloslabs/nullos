#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

// ============================================================================
// USB Speeds
// ============================================================================
#define USB_SPEED_LOW       0   // 1.5 Mbps (USB 1.0)
#define USB_SPEED_FULL      1   // 12 Mbps  (USB 1.1)
#define USB_SPEED_HIGH      2   // 480 Mbps (USB 2.0)
#define USB_SPEED_SUPER     3   // 5 Gbps   (USB 3.0)
#define USB_SPEED_SUPER_PLUS 4  // 10 Gbps  (USB 3.1)

// ============================================================================
// USB PCI Class Codes
// ============================================================================
#define USB_PCI_CLASS       0x0C
#define USB_PCI_SUBCLASS    0x03
#define USB_PROGIF_UHCI     0x00
#define USB_PROGIF_OHCI     0x10
#define USB_PROGIF_EHCI     0x20
#define USB_PROGIF_XHCI     0x30

// ============================================================================
// USB Standard Request Codes
// ============================================================================
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SET_PROTOCOL      0x0B  // HID class-specific

// ============================================================================
// USB Descriptor Types
// ============================================================================
#define USB_DESC_DEVICE           1
#define USB_DESC_CONFIGURATION    2
#define USB_DESC_STRING           3
#define USB_DESC_INTERFACE        4
#define USB_DESC_ENDPOINT         5
#define USB_DESC_HID              0x21

// ============================================================================
// USB Request Type Fields
// ============================================================================
#define USB_REQTYPE_DIR_OUT       0x00
#define USB_REQTYPE_DIR_IN        0x80
#define USB_REQTYPE_STANDARD      0x00
#define USB_REQTYPE_CLASS         0x20
#define USB_REQTYPE_VENDOR        0x40
#define USB_REQTYPE_DEVICE        0x00
#define USB_REQTYPE_INTERFACE     0x01
#define USB_REQTYPE_ENDPOINT      0x02

// ============================================================================
// USB Endpoint Types
// ============================================================================
#define USB_EP_TYPE_CONTROL       0
#define USB_EP_TYPE_ISOCHRONOUS   1
#define USB_EP_TYPE_BULK          2
#define USB_EP_TYPE_INTERRUPT     3

// ============================================================================
// USB HID Constants
// ============================================================================
#define USB_HID_CLASS             3
#define USB_HID_SUBCLASS_BOOT     1
#define USB_HID_PROTOCOL_KEYBOARD 1
#define USB_HID_PROTOCOL_MOUSE    2

// ============================================================================
// USB Setup Packet
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

// ============================================================================
// USB Device Descriptor
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

// ============================================================================
// USB Configuration Descriptor
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

// ============================================================================
// USB Interface Descriptor
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_descriptor_t;

// ============================================================================
// USB Endpoint Descriptor
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

// ============================================================================
// USB Device (runtime state)
// ============================================================================
#define USB_MAX_ENDPOINTS 16

typedef struct usb_device {
    uint8_t  address;        // Device address on the bus (1-127)
    uint8_t  speed;          // USB_SPEED_*
    uint8_t  max_packet_size; // For EP0
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  port_id;        // Physical port identifier on the HCD
    uint8_t  interrupt_toggle; // DATA0/DATA1 for interrupt transfers
    void    *hcd_data;       // HCD-specific per-device data
} usb_device_t;

// ============================================================================
// USB Host Controller Driver (vtable)
// ============================================================================
typedef struct usb_hcd {
    const char *name;
    int (*control_transfer)(struct usb_hcd *hcd, usb_device_t *dev,
                            usb_setup_packet_t *setup, void *data, uint16_t length);
    int (*interrupt_transfer)(struct usb_hcd *hcd, usb_device_t *dev,
                              uint8_t endpoint, void *data, uint16_t length);
    int (*bulk_transfer)(struct usb_hcd *hcd, usb_device_t *dev,
                         uint8_t endpoint, void *data, uint16_t length);
    void *hcd_data;          // Per-controller private data
} usb_hcd_t;

// ============================================================================
// Global USB device registry
// ============================================================================
#define USB_MAX_DEVICES 32

extern usb_device_t usb_devices[USB_MAX_DEVICES];
extern int usb_device_count;
extern usb_hcd_t *usb_active_hcd;

void register_usb_hcd(usb_hcd_t *hcd);
void poll_usb_hcds(void);
