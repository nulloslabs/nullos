#pragma once

#include <stdbool.h>
#include <io/pci.h>
#include <io/usb.h>

#define HID_MOD_LCTRL  (1 << 0)
#define HID_MOD_LSHIFT (1 << 1)
#define HID_MOD_LALT   (1 << 2)
#define HID_MOD_LGUI   (1 << 3)
#define HID_MOD_RCTRL  (1 << 4)
#define HID_MOD_RSHIFT (1 << 5)
#define HID_MOD_RALT   (1 << 6)
#define HID_MOD_RGUI   (1 << 7)

#define USB_KEYBOARD_REPEAT_DELAY_TICKS    125
#define USB_KEYBOARD_REPEAT_INTERVAL_TICKS 8

// Keyboard device entry (also used by HCI drivers for polling)
typedef struct {
    usb_device_t *dev;
    usb_hcd_t *hcd;
    uint8_t *report_buf;      // Current buffer (being read by software)
    uint8_t *report_buf_next; // Next buffer (being written by hardware, ping-pong)
    uint8_t prev_report[8];   // Per-keyboard previous HID report
    uint8_t repeat_key;       // Per-keyboard HID usage ID being repeated
    int repeat_timer;         // Per-keyboard repeat timer counter
    uint8_t interface_number; // HID interface receiving class requests
    uint8_t endpoint_number;  // Interrupt-IN endpoint from its descriptor
    uint8_t pending_leds;     // HID boot-keyboard output report
    uint8_t applied_leds;
    bool leds_dirty;
} usb_keyboard_entry_t;

extern usb_keyboard_entry_t *kbd_list;
extern int kbd_max_total;
extern int kbd_total;

void usb_keyboard_process_report(uint8_t *report, int kbd_index);
void poll_usb_keyboard(void);
int kbd_find_index(usb_device_t *dev);
void set_usb_keyboard_leds(uint8_t leds);
void remove_usb_keyboard(usb_hcd_t *hcd, uint8_t port_id);
void init_usb_keyboard(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id);
