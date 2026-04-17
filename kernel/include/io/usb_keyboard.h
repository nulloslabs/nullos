#pragma once

#include <io/pci.h>
#include <io/usb.h>

#define MAX_USB_KEYBOARDS 4

// Keyboard device entry (also used by HCI drivers for polling)
typedef struct {
    usb_device_t *dev;
    usb_hcd_t *hcd;
    uint8_t *report_buf;      // Current buffer (being read by software)
    uint8_t *report_buf_next; // Next buffer (being written by hardware, ping-pong)
    uint8_t prev_report[8];   // Per-keyboard previous HID report
    uint8_t repeat_key;       // Per-keyboard HID usage ID being repeated
    int repeat_timer;         // Per-keyboard repeat timer counter
} kbd_entry_t;

extern kbd_entry_t kbd_list[MAX_USB_KEYBOARDS];
extern int kbd_total;

void init_usb_keyboard(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id);
void usb_keyboard_process_report(uint8_t *report, int kbd_index);
void poll_usb_keyboard(void);
int kbd_find_index(usb_device_t *dev);
