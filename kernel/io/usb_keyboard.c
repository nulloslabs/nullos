#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/log.h>
#include <main/string.h>
#include <io/usb_keyboard.h>
#include <io/usb.h>
#include <io/keyboard.h>
#include <io/time.h>
#include <io/tty.h>
#include <mm/mm.h>
static const uint8_t hid_to_scancode[256] = {
    [0x00] = 0x00,  // Reserved (no event)
    [0x01] = 0x00,  // ErrorRollOver
    [0x02] = 0x00,  // POSTFail
    [0x03] = 0x00,  // ErrorUndefined
    [0x04] = 0x1E,  // a
    [0x05] = 0x30,  // b
    [0x06] = 0x2E,  // c
    [0x07] = 0x20,  // d
    [0x08] = 0x12,  // e
    [0x09] = 0x21,  // f
    [0x0A] = 0x22,  // g
    [0x0B] = 0x23,  // h
    [0x0C] = 0x17,  // i
    [0x0D] = 0x24,  // j
    [0x0E] = 0x25,  // k
    [0x0F] = 0x26,  // l
    [0x10] = 0x32,  // m
    [0x11] = 0x31,  // n
    [0x12] = 0x18,  // o
    [0x13] = 0x19,  // p
    [0x14] = 0x10,  // q
    [0x15] = 0x13,  // r
    [0x16] = 0x1F,  // s
    [0x17] = 0x14,  // t
    [0x18] = 0x16,  // u
    [0x19] = 0x2F,  // v
    [0x1A] = 0x11,  // w
    [0x1B] = 0x2D,  // x
    [0x1C] = 0x15,  // y
    [0x1D] = 0x2C,  // z
    [0x1E] = 0x02,  // 1
    [0x1F] = 0x03,  // 2
    [0x20] = 0x04,  // 3
    [0x21] = 0x05,  // 4
    [0x22] = 0x06,  // 5
    [0x23] = 0x07,  // 6
    [0x24] = 0x08,  // 7
    [0x25] = 0x09,  // 8
    [0x26] = 0x0A,  // 9
    [0x27] = 0x0B,  // 0
    [0x28] = 0x1C,  // Enter
    [0x29] = 0x01,  // Escape
    [0x2A] = 0x0E,  // Backspace
    [0x2B] = 0x0F,  // Tab
    [0x2C] = 0x39,  // Space
    [0x2D] = 0x0C,  // -
    [0x2E] = 0x0D,  // =
    [0x2F] = 0x1A,  // [
    [0x30] = 0x1B,  // ]
    [0x31] = 0x2B,  // backslash
    [0x33] = 0x27,  // ;
    [0x34] = 0x28,  // '
    [0x35] = 0x29,  // `
    [0x36] = 0x33,  // ,
    [0x37] = 0x34,  // .
    [0x38] = 0x35,  // /
    [0x39] = 0x3A,  // Caps Lock
    [0x3A] = 0x3B,  // F1
    [0x3B] = 0x3C,  // F2
    [0x3C] = 0x3D,  // F3
    [0x3D] = 0x3E,  // F4
    [0x3E] = 0x3F,  // F5
    [0x3F] = 0x40,  // F6
    [0x40] = 0x41,  // F7
    [0x41] = 0x42,  // F8
    [0x42] = 0x43,  // F9
    [0x43] = 0x44,  // F10
    [0x44] = 0x57,  // F11
    [0x45] = 0x58,  // F12
    [0x49] = 0x68,  // Insert   -> SC_INSERT
    [0x4A] = 0x64,  // Home     -> SC_HOME
    [0x4B] = 0x66,  // Page Up  -> SC_PGUP
    [0x4C] = 0x69,  // Delete   -> SC_DELETE
    [0x4D] = 0x65,  // End      -> SC_END
    [0x4E] = 0x67,  // Page Down-> SC_PGDN
    [0x4F] = 0x62,  // Right    -> SC_RIGHT
    [0x50] = 0x63,  // Left     -> SC_LEFT
    [0x51] = 0x61,  // Down     -> SC_DOWN
    [0x52] = 0x60,  // Up       -> SC_UP
    [0x53] = 0x45,  // Num Lock
    // Modifier keys (usage IDs 0xE0-0xE7)
    [0xE0] = 0x1D,  // Left Control
    [0xE1] = 0x2A,  // Left Shift
    [0xE2] = 0x38,  // Left Alt
    [0xE3] = 0x5B,  // Left GUI
    [0xE4] = 0x1D,  // Right Control
    [0xE5] = 0x36,  // Right Shift
    [0xE6] = 0x38,  // Right Alt
    [0xE7] = 0x5C,  // Right GUI
};

#define HID_MOD_LCTRL   (1 << 0)
#define HID_MOD_LSHIFT  (1 << 1)
#define HID_MOD_LALT    (1 << 2)
#define HID_MOD_LGUI    (1 << 3)
#define HID_MOD_RCTRL   (1 << 4)
#define HID_MOD_RSHIFT  (1 << 5)
#define HID_MOD_RALT    (1 << 6)
#define HID_MOD_RGUI    (1 << 7)
#define USB_KEYBOARD_REPEAT_DELAY_TICKS 125
#define USB_KEYBOARD_REPEAT_INTERVAL_TICKS 8

kbd_entry_t *kbd_list      = NULL;
int          kbd_max_total = 0;
int          kbd_total     = 0;

int kbd_find_index(usb_device_t *dev) {
    if (!kbd_list) return -1;
    for (int i = 0; i < kbd_total; i++) {
        if (kbd_list[i].dev == dev) return i;
    }
    return -1;
}

void set_usb_keyboard_leds(uint8_t leds) {
    leds &= 0x07;
    for (int i = 0; i < kbd_total; i++) {
        kbd_list[i].pending_leds = leds;
        kbd_list[i].leds_dirty = true;
    }
}

void usb_keyboard_process_report(uint8_t *report, int kbd_index) {
    if (!report || kbd_index < 0 || kbd_index >= kbd_total) return;
    uint8_t *prev_report = kbd_list[kbd_index].prev_report;
    if (!prev_report) return;

    uint8_t modifiers      = report[0];
    uint8_t prev_modifiers = prev_report[0];

    int caps_is_pressed = 0;
    int caps_was_pressed = 0;
    for (int i = 2; i < 8; i++) {
        if (report[i]      == 0x39) caps_is_pressed  = 1;
        if (prev_report[i] == 0x39) caps_was_pressed = 1;
    }
    (void)caps_was_pressed;

    uint8_t curr_shift = modifiers      & (HID_MOD_LSHIFT | HID_MOD_RSHIFT);
    uint8_t prev_shift = prev_modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT);

    if (caps_is_pressed) {
        curr_shift = 0;
        prev_shift = 0;
    }

    if (curr_shift && !prev_shift) {
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) {
            key_buffer[key_head] = 0x2A;
            key_head = next;
        }
        tty_process_scancode(0x2A);
    } else if (!curr_shift && prev_shift) {
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) {
            key_buffer[key_head] = 0xAA;
            key_head = next;
        }
        tty_process_scancode(0xAA);
    }

    // Alt modifier tracking (same pattern as Shift)
    {
        uint8_t curr_alt = modifiers & (HID_MOD_LALT | HID_MOD_RALT);
        uint8_t prev_alt = prev_modifiers & (HID_MOD_LALT | HID_MOD_RALT);
        if (curr_alt && !prev_alt) {
            uint32_t next = (key_head + 1) & 127;
            if (next != key_tail) { key_buffer[key_head] = 0x38; key_head = next; }
            tty_process_scancode(0x38);
        } else if (!curr_alt && prev_alt) {
            uint32_t next = (key_head + 1) & 127;
            if (next != key_tail) { key_buffer[key_head] = 0xB8; key_head = next; }
            tty_process_scancode(0xB8);
        }
    }

    // Control modifier tracking (same pattern as Shift/Alt).
    // Emits PS/2 set-1 scancode 0x1D on press, 0x9D on release, which is
    // what keyboard.c watches to set/clear ctrl_pressed.  Without this
    // block, USB keyboards could never produce Ctrl+key combos even though
    // HID_MOD_LCTRL/HID_MOD_RCTRL were defined above.
    {
        uint8_t curr_ctrl = modifiers & (HID_MOD_LCTRL | HID_MOD_RCTRL);
        uint8_t prev_ctrl = prev_modifiers & (HID_MOD_LCTRL | HID_MOD_RCTRL);
        if (curr_ctrl && !prev_ctrl) {
            uint32_t next = (key_head + 1) & 127;
            if (next != key_tail) { key_buffer[key_head] = 0x1D; key_head = next; }
            tty_process_scancode(0x1D);
        } else if (!curr_ctrl && prev_ctrl) {
            uint32_t next = (key_head + 1) & 127;
            if (next != key_tail) { key_buffer[key_head] = 0x9D; key_head = next; }
            tty_process_scancode(0x9D);
        }
    }

    // Newly pressed keys
    for (int i = 2; i < 8; i++) {
        uint8_t usage = report[i];
        if (usage == 0) continue;

        int was_pressed = 0;
        for (int j = 2; j < 8; j++) {
            if (prev_report[j] == usage) { was_pressed = 1; break; }
        }

        if (!was_pressed) {
            uint8_t scancode = hid_to_scancode[usage];
            if (scancode) {
                uint32_t next = (key_head + 1) & 127;
                if (next != key_tail) {
                    key_buffer[key_head] = scancode;
                    key_head = next;
                }
                handle_keyboard_lock_scancode(scancode);
                tty_process_scancode(scancode);
            }
        }
    }

    // Released keys
    for (int i = 2; i < 8; i++) {
        uint8_t usage = prev_report[i];
        if (usage == 0) continue;

        int still_pressed = 0;
        for (int j = 2; j < 8; j++) {
            if (report[j] == usage) { still_pressed = 1; break; }
        }

        if (!still_pressed) {
            uint8_t scancode = hid_to_scancode[usage];
            if (scancode) {
                uint32_t next = (key_head + 1) & 127;
                if (next != key_tail) {
                    key_buffer[key_head] = scancode | 0x80;
                    key_head = next;
                }
                tty_process_scancode(scancode | 0x80);
            }
        }
    }

    memcpy(prev_report, report, 8);
}

void poll_usb_keyboard(void) {
    for (int k = 0; k < kbd_total; k++) {
        if (!kbd_list[k].hcd || !kbd_list[k].dev)
            continue;

        if (kbd_list[k].leds_dirty) {
            usb_setup_packet_t setup = {
                .bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_CLASS |
                                 USB_REQTYPE_INTERFACE,
                .bRequest = USB_REQ_SET_REPORT,
                .wValue = 0x0200,
                .wIndex = kbd_list[k].interface_number,
                .wLength = 1,
            };
            uint8_t leds = kbd_list[k].pending_leds;
            if (kbd_list[k].hcd->control_transfer(kbd_list[k].hcd,
                                                  kbd_list[k].dev, &setup,
                                                  &leds, 1) >= 0) {
                kbd_list[k].applied_leds = leds;
                kbd_list[k].leds_dirty =
                    kbd_list[k].pending_leds != kbd_list[k].applied_leds;
            }
        }

        uint8_t *pr = kbd_list[k].prev_report;
        uint8_t first_key = 0;
        for (int i = 2; i < 8; i++) {
            if (pr[i] != 0) { first_key = pr[i]; break; }
        }

        if (first_key != 0 && first_key == kbd_list[k].repeat_key) {
            kbd_list[k].repeat_timer++;
            // USB boot keyboards do not generate typematic reports, so use
            // the 250 Hz USB poll to provide a 500 ms delay and ~31 cps rate.
            if (kbd_list[k].repeat_timer >= USB_KEYBOARD_REPEAT_DELAY_TICKS) {
                if ((kbd_list[k].repeat_timer - USB_KEYBOARD_REPEAT_DELAY_TICKS) % USB_KEYBOARD_REPEAT_INTERVAL_TICKS == 0) {
                    uint8_t scancode = hid_to_scancode[first_key];
                    if (scancode) {
                        uint32_t next = (key_head + 1) & 127;
                        if (next != key_tail) {
                            key_buffer[key_head] = scancode;
                            key_head = next;
                        }
                        tty_process_scancode(scancode);
                    }
                }
            }
        } else {
            kbd_list[k].repeat_key   = first_key;
            kbd_list[k].repeat_timer = 0;
        }
    }
}

// Find a boot-keyboard interface and its interrupt-IN endpoint.
static int usb_find_boot_keyboard_interface(uint8_t *buf, uint16_t total_len, uint8_t *endpoint_number, uint16_t *max_packet) {
    uint16_t offset = 0;
    int interface_number = -1;
    while (offset + 2 <= total_len) {
        uint8_t desc_len  = buf[offset];
        uint8_t desc_type = buf[offset + 1];
        if (desc_len < 2 || offset + desc_len > total_len) break;

        if (desc_type == USB_DESC_INTERFACE && offset + 9 <= total_len) {
            uint8_t iface_class    = buf[offset + 5];
            uint8_t iface_subclass = buf[offset + 6];
            uint8_t iface_protocol = buf[offset + 7];
            interface_number = -1;
            if (iface_class    == USB_HID_CLASS             &&
                iface_subclass == USB_HID_SUBCLASS_BOOT     &&
                iface_protocol == USB_HID_PROTOCOL_KEYBOARD &&
                buf[offset + 3] == 0)
                interface_number = buf[offset + 2];
        } else if (interface_number >= 0 &&
                   desc_type == USB_DESC_ENDPOINT &&
                   offset + sizeof(usb_endpoint_descriptor_t) <= total_len) {
            usb_endpoint_descriptor_t *endpoint =
                (usb_endpoint_descriptor_t *)&buf[offset];
            uint16_t packet_size = endpoint->wMaxPacketSize & 0x7FF;
            if ((endpoint->bEndpointAddress & 0x80) &&
                (endpoint->bmAttributes & 3) == USB_EP_TYPE_INTERRUPT &&
                (endpoint->bEndpointAddress & 0x0F) != 0 &&
                packet_size >= 8 && packet_size <= 64) {
                *endpoint_number = endpoint->bEndpointAddress & 0x0F;
                *max_packet = packet_size;
                return interface_number;
            }
        }
        offset += desc_len;
    }
    return -1;
}

typedef struct {
    uint8_t *raw;
    uint8_t *page;
} usb_dma_scratch_t;

static int usb_alloc_dma_scratch(usb_dma_scratch_t *scratch) {
    if (!scratch) return -1;
    scratch->raw  = NULL;
    scratch->page = NULL;

    uint8_t *raw = (uint8_t *)malloc(8192);
    if (!raw) return -1;

    uintptr_t aligned = ((uintptr_t)raw + 4095ULL) & ~4095ULL;
    if (aligned + 4096ULL > (uintptr_t)raw + 8192ULL) {
        free(raw);
        return -1;
    }

    scratch->raw  = raw;
    scratch->page = (uint8_t *)aligned;
    memset(scratch->page, 0, 4096);
    return 0;
}

static void usb_free_dma_scratch(usb_dma_scratch_t *scratch) {
    if (!scratch) return;
    if (scratch->raw) free(scratch->raw);
    scratch->raw  = NULL;
    scratch->page = NULL;
}

static int usb_control_transfer_retry(usb_hcd_t *hcd, usb_device_t *dev,
                                      usb_setup_packet_t *setup, void *data,
                                      uint16_t length, int retries,
                                      uint64_t retry_delay_ms) {
    if (retries < 1) retries = 1;
    int ret = -1;
    for (int i = 0; i < retries; i++) {
        ret = hcd->control_transfer(hcd, dev, setup, data, length);
        if (ret >= 0) return ret;
        if (i + 1 < retries && retry_delay_ms) sleep(retry_delay_ms);
    }
    return ret;
}

static int kbd_list_grow(void) {
    int new_max = kbd_max_total + 32;
    kbd_entry_t *new_list = malloc(sizeof(kbd_entry_t) * new_max);
    if (!new_list) return -1;
    memset(new_list, 0, sizeof(kbd_entry_t) * new_max);
    if (kbd_list) {
        memcpy(new_list, kbd_list, sizeof(kbd_entry_t) * kbd_total);
        free(kbd_list);
    }
    kbd_list      = new_list;
    kbd_max_total = new_max;
    return 0;
}

void remove_usb_keyboard(usb_hcd_t *hcd, uint8_t port_id) {
    for (int i = 0; i < kbd_total; i++) {
        if (kbd_list[i].hcd != hcd || !kbd_list[i].dev) continue;
        if (kbd_list[i].dev->port_id != port_id) continue;

        usb_device_t *dev = kbd_list[i].dev;
        free(kbd_list[i].report_buf);
        free(kbd_list[i].report_buf_next);
        unregister_usb_device(dev);
        kbd_total--;
        if (i < kbd_total) memmove(&kbd_list[i], &kbd_list[i + 1], (kbd_total - i) * sizeof(kbd_entry_t));
        memset(&kbd_list[kbd_total], 0, sizeof(kbd_entry_t));
        return;
    }
}

// USB boot-keyboard initialisation.
// Full enumeration: GET_DESCRIPTOR → SET_ADDRESS → verify boot keyboard →
// SET_CONFIGURATION → SET_PROTOCOL 0.
void init_usb_keyboard(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id) {
    if (!hcd) return;

    // Init kbd_list on first call
    if (!kbd_list) {
        kbd_max_total = 32;
        kbd_list = malloc(sizeof(kbd_entry_t) * kbd_max_total);
        if (!kbd_list) return;
        memset(kbd_list, 0, sizeof(kbd_entry_t) * kbd_max_total);
    }

    // Duplicate port check
    for (int i = 0; i < kbd_total; i++) {
        if (kbd_list[i].hcd == hcd && kbd_list[i].dev &&
            kbd_list[i].dev->port_id == port_id) return;
    }

    usb_device_t *dev = usb_allocate_device();
    if (!dev) {
        log("usb keyboard: failed to allocate device slot\n");
        return;
    }

    // Allocate report ping-pong buffers upfront
    uint8_t *rbuf  = malloc(8);
    uint8_t *rbuf2 = malloc(8);
    if (!rbuf || !rbuf2) {
        log("usb keyboard: failed to allocate report buffers for port %d\n", port_id);
        if (rbuf)  free(rbuf);
        if (rbuf2) free(rbuf2);
        unregister_usb_device(dev);
        return;
    }
    memset(rbuf,  0, 8);
    memset(rbuf2, 0, 8);

    usb_dma_scratch_t dma = {0};
    if (usb_alloc_dma_scratch(&dma) < 0) {
        log("usb keyboard: failed to allocate dma scratch for port %d\n", port_id);
        free(rbuf);
        free(rbuf2);
        unregister_usb_device(dev);
        return;
    }

    usb_setup_packet_t      *setup   = (usb_setup_packet_t *)(dma.page + 0);
    uint8_t                 *desc_buf = dma.page + 64;
    uint8_t                 *cfg_buf  = dma.page + 128;

    int new_address = usb_allocate_address(0);
    if (new_address < 0) {
        log("usb keyboard: no free device addresses left for port %d\n", port_id);
        goto fail_probe;
    }

    dev->address         = 0;
    dev->speed           = speed;
    dev->max_packet_size = 8; // UHCI is always full-speed or low-speed
    dev->port_id         = port_id;
    dev->interrupt_toggle = 0;
    dev->hcd_data        = NULL;
    memset(desc_buf, 0, 64);

    // ---- Step 1: GET_DESCRIPTOR (Device, first 8 bytes at address 0) ----
    setup->bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup->wValue        = (USB_DESC_DEVICE << 8) | 0;
    setup->wIndex        = 0;
    setup->wLength       = 8;
    if (usb_control_transfer_retry(hcd, dev, setup, desc_buf, 8, 4, 10) < 0) {
        usb_release_address(new_address);
        goto fail_probe;
    }
    uint8_t ep0_mps = desc_buf[7];
    if (ep0_mps != 8 && ep0_mps != 16 && ep0_mps != 32 && ep0_mps != 64)
        ep0_mps = 8; // UHCI default
    dev->max_packet_size = ep0_mps;

    // ---- Step 2: SET_ADDRESS ----
    setup->bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_SET_ADDRESS;
    setup->wValue        = new_address;
    setup->wIndex        = 0;
    setup->wLength       = 0;
    if (usb_control_transfer_retry(hcd, dev, setup, NULL, 0, 3, 10) < 0) {
        usb_release_address(new_address);
        goto fail_probe;
    }
    dev->address = new_address;
    sleep(20);

    // ---- Step 3: GET_DESCRIPTOR (Device, 18 bytes) ----
    memset(desc_buf, 0, 64);
    setup->bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup->wValue        = (USB_DESC_DEVICE << 8) | 0;
    setup->wIndex        = 0;
    setup->wLength       = 18;
    if (usb_control_transfer_retry(hcd, dev, setup, desc_buf, 18, 5, 20) < 0) {
        goto fail_probe_with_addr;
    }

    usb_device_descriptor_t *ddev = (usb_device_descriptor_t *)desc_buf;
    dev->vendor_id  = ddev->idVendor;
    dev->product_id = ddev->idProduct;

    if (ddev->bDeviceClass != 0x00 && ddev->bDeviceClass != USB_HID_CLASS) {
        goto fail_probe_with_addr;
    }

    // ---- Step 4a: GET_DESCRIPTOR (Configuration header, 9 bytes) ----
    memset(cfg_buf, 0, 64);
    setup->bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup->wValue        = (USB_DESC_CONFIGURATION << 8) | 0;
    setup->wIndex        = 0;
    setup->wLength       = sizeof(usb_config_descriptor_t);
    if (usb_control_transfer_retry(hcd, dev, setup, cfg_buf,
                                   sizeof(usb_config_descriptor_t), 4, 10) < 0) {
        goto fail_probe_with_addr;
    }

    // ---- Step 4b: GET_DESCRIPTOR (Configuration body) ----
    usb_config_descriptor_t *dcfg = (usb_config_descriptor_t *)cfg_buf;
    uint16_t cfg_read_len = dcfg->wTotalLength;
    if (cfg_read_len < sizeof(usb_config_descriptor_t))
        cfg_read_len = sizeof(usb_config_descriptor_t);
    if (cfg_read_len > 512) cfg_read_len = 512;

    if (cfg_read_len > sizeof(usb_config_descriptor_t)) {
        memset(cfg_buf, 0, 512);
        setup->wLength = cfg_read_len;
        if (usb_control_transfer_retry(hcd, dev, setup, cfg_buf,
                                       cfg_read_len, 4, 10) < 0) {
            goto fail_probe_with_addr;
        }
    }

    dcfg = (usb_config_descriptor_t *)cfg_buf;
    uint16_t total_len = dcfg->wTotalLength;
    if (total_len > cfg_read_len) total_len = cfg_read_len;

    uint8_t endpoint_number = 0;
    uint16_t endpoint_max_packet = 0;
    int interface_number = usb_find_boot_keyboard_interface(cfg_buf, total_len, &endpoint_number, &endpoint_max_packet);
    if (interface_number < 0) {
        goto fail_probe_with_addr;
    }

    // ---- Step 5: SET_CONFIGURATION ----
    setup->bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_SET_CONFIGURATION;
    setup->wValue        = dcfg->bConfigurationValue;
    setup->wIndex        = 0;
    setup->wLength       = 0;
    if (hcd->control_transfer(hcd, dev, setup, NULL, 0) < 0) {
        log("usb keyboard: port %d: set_configuration failed\n", port_id);
        goto fail_probe_with_addr;
    }

    // ---- Step 6: SET_PROTOCOL 0 (Boot Protocol) ----
    setup->bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE;
    setup->bRequest      = USB_REQ_SET_PROTOCOL;
    setup->wValue        = 0; // Boot Protocol
    setup->wIndex        = (uint16_t)interface_number;
    setup->wLength       = 0;
    if (hcd->control_transfer(hcd, dev, setup, NULL, 0) < 0) {
        log("usb keyboard: port %d: set_protocol failed\n", port_id);
        goto fail_probe_with_addr;
    }

    // ---- Confirmed and configured boot keyboard — commit resources ----
    if (kbd_total >= kbd_max_total && kbd_list_grow() < 0) {
        log("usb keyboard: port %d: kbd_list grow failed, dropping keyboard\n", port_id);
        goto fail_probe_with_addr;
    }

    kbd_list[kbd_total].dev              = dev;
    kbd_list[kbd_total].hcd              = hcd;
    kbd_list[kbd_total].report_buf       = rbuf;
    kbd_list[kbd_total].report_buf_next  = rbuf2;
    memset(kbd_list[kbd_total].prev_report, 0, 8);
    kbd_list[kbd_total].repeat_key       = 0;
    kbd_list[kbd_total].repeat_timer     = 0;
    kbd_list[kbd_total].interface_number = (uint8_t)interface_number;
    kbd_list[kbd_total].endpoint_number  = endpoint_number;
    dev->interrupt_max_packet            = endpoint_max_packet;
    kbd_list[kbd_total].pending_leds     = get_keyboard_led_state();
    kbd_list[kbd_total].applied_leds     = 0;
    kbd_list[kbd_total].leds_dirty       = true;
    kbd_total++;

    usb_free_dma_scratch(&dma);
    return;

fail_probe_with_addr:
fail_probe:
    unregister_usb_device(dev);
    free(rbuf);
    free(rbuf2);
    usb_free_dma_scratch(&dma);
}
