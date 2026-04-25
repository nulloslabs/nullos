#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/usb_keyboard.h>
#include <io/usb.h>
#include <io/keyboard.h>
#include <io/terminal.h>
#include <io/ehci.h>  // For ehci_poll_keyboards
#include <io/hpet.h>
#include <mm/mm.h>
#include <main/string.h>

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
    [0x2B] = 0x00,  // Tab (Disabled)
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

// HID modifier bit positions (byte 0 of boot keyboard report)
#define HID_MOD_LCTRL   (1 << 0)
#define HID_MOD_LSHIFT  (1 << 1)
#define HID_MOD_LALT    (1 << 2)
#define HID_MOD_LGUI    (1 << 3)
#define HID_MOD_RCTRL   (1 << 4)
#define HID_MOD_RSHIFT  (1 << 5)
#define HID_MOD_RALT    (1 << 6)
#define HID_MOD_RGUI    (1 << 7)

// HID usage IDs for modifier keys
#define HID_USAGE_LCTRL   0xE0
#define HID_USAGE_LSHIFT  0xE1
#define HID_USAGE_LALT    0xE2
#define HID_USAGE_LGUI    0xE3
#define HID_USAGE_RCTRL   0xE4
#define HID_USAGE_RSHIFT  0xE5
#define HID_USAGE_RALT    0xE6
#define HID_USAGE_RGUI    0xE7

// Track active keyboard devices
#define MAX_USB_KEYBOARDS 4
kbd_entry_t kbd_list[MAX_USB_KEYBOARDS];
int kbd_total = 0;

// Find keyboard index by device pointer
int kbd_find_index(usb_device_t *dev) {
    for (int i = 0; i < kbd_total; i++) {
        if (kbd_list[i].dev == dev) return i;
    }
    return -1;
}

// ============================================================================
// Process a HID boot keyboard report (8 bytes)
// Byte 0: Modifier keys (bitmask)
// Byte 1: Reserved
// Bytes 2-7: Up to 6 simultaneous key codes (HID usage IDs)
// ============================================================================
void usb_keyboard_process_report(uint8_t *report, int kbd_index) {
    if (!report || kbd_index < 0 || kbd_index >= kbd_total) return;
    uint8_t *prev_report = kbd_list[kbd_index].prev_report;
    
    // Validate prev_report pointer
    if (!prev_report) return;

    uint8_t modifiers = report[0];
    uint8_t prev_modifiers = prev_report[0];

    // Check if Caps Lock is CURRENTLY pressed and if it was JUST pressed (transition)
    int caps_is_pressed = 0;
    int caps_was_pressed = 0;
    for (int i = 2; i < 8; i++) {
        if (report[i] == 0x39) caps_is_pressed = 1;
        if (prev_report[i] == 0x39) caps_was_pressed = 1;
    }

    // Handle shift key press/release via modifier bits
    uint8_t curr_shift = modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT);
    uint8_t prev_shift = prev_modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT);
    
    // If Caps Lock is currently pressed, IGNORE shift modifier completely
    // (some keyboards incorrectly report shift when Caps Lock is active)
    if (caps_is_pressed) {
        curr_shift = 0;
        prev_shift = 0;
    }
    
    if (curr_shift && !prev_shift) {
        // Shift pressed — inject PS/2 left shift make code
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) {
            key_buffer[key_head] = 0x2A; // Left shift make
            key_head = next;
        }
    } else if (!curr_shift && prev_shift) {
        // Shift released
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) {
            key_buffer[key_head] = 0xAA; // Left shift break (0x2A | 0x80)
            key_head = next;
        }
    }

    // Find newly pressed keys (in current report but not in previous)
    for (int i = 2; i < 8; i++) {
        uint8_t usage = report[i];
        if (usage == 0) continue;

        // Check if this key was already pressed in previous report
        int was_pressed = 0;
        for (int j = 2; j < 8; j++) {
            if (prev_report[j] == usage) {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed) {
            // New key press — convert to PS/2 scancode and inject
            uint8_t scancode = hid_to_scancode[usage];
            if (scancode) {
                uint32_t next = (key_head + 1) & 127;
                if (next != key_tail) {
                    key_buffer[key_head] = scancode;
                    key_head = next;
                }
            }
        }
    }

    // Find released keys (in previous report but not in current)
    for (int i = 2; i < 8; i++) {
        uint8_t usage = prev_report[i];
        if (usage == 0) continue;

        int still_pressed = 0;
        for (int j = 2; j < 8; j++) {
            if (report[j] == usage) {
                still_pressed = 1;
                break;
            }
        }

        if (!still_pressed) {
            // Key released — inject break code (scancode | 0x80)
            uint8_t scancode = hid_to_scancode[usage];
            if (scancode) {
                uint32_t next = (key_head + 1) & 127;
                if (next != key_tail) {
                    key_buffer[key_head] = scancode | 0x80;
                    key_head = next;
                }
            }
        }
    }

    // Save current report as previous (track held keys)
    memcpy(prev_report, report, 8);
}

void poll_usb_keyboard(void) {
    // Per-keyboard software repeat: synthesize PS/2-style repeat from each keyboard's
    // prev_report independently so controllers don't interfere with each other.
    for (int k = 0; k < kbd_total; k++) {
        // Validate keyboard entry before accessing
        if (!kbd_list[k].hcd || !kbd_list[k].dev || !kbd_list[k].prev_report) {
            continue;
        }
        
        uint8_t *pr = kbd_list[k].prev_report;
        uint8_t first_key = 0;
        for (int i = 2; i < 8; i++) {
            if (pr[i] != 0) {
                first_key = pr[i];
                break;
            }
        }

        if (first_key != 0 && first_key == kbd_list[k].repeat_key) {
            kbd_list[k].repeat_timer++;
            // Delay before repeat: 500ms (125 polls at 4ms)
            // Repeat rate: 32ms (every 8th poll after delay)
            if (kbd_list[k].repeat_timer >= 125) {
                if ((kbd_list[k].repeat_timer - 125) % 8 == 0) {
                    uint8_t scancode = hid_to_scancode[first_key];
                    if (scancode) {
                        uint32_t next = (key_head + 1) & 127;
                        if (next != key_tail) {
                            key_buffer[key_head] = scancode;
                            key_head = next;
                        }
                    }
                }
            }
        } else {
            kbd_list[k].repeat_key = first_key;
            kbd_list[k].repeat_timer = 0;
        }
    }
}

// ============================================================================
// Descriptor-based device identification
// ============================================================================

// Walk a raw descriptor buffer looking for the first Interface descriptor (type 4).
// Returns 1 if a boot-protocol keyboard interface is found, 0 otherwise.
static int usb_find_boot_keyboard_interface(uint8_t *buf, uint16_t total_len) {
    uint16_t offset = 0;
    while (offset + 2 <= total_len) {
        uint8_t desc_len  = buf[offset];
        uint8_t desc_type = buf[offset + 1];
        if (desc_len < 2) break; // Malformed descriptor — stop walking

        if (desc_type == USB_DESC_INTERFACE && offset + 9 <= total_len) {
            uint8_t iface_class    = buf[offset + 5];
            uint8_t iface_subclass = buf[offset + 6];
            uint8_t iface_protocol = buf[offset + 7];
            if (iface_class    == USB_HID_CLASS             &&
                iface_subclass == USB_HID_SUBCLASS_BOOT     &&
                iface_protocol == USB_HID_PROTOCOL_KEYBOARD) {
                return 1;
            }
            // Not a keyboard interface — stop (we only care about interface 0)
            return 0;
        }
        offset += desc_len;
    }
    return 0;
}

typedef struct {
    uint8_t *raw;
    uint8_t *page;
} usb_dma_scratch_t;

static int usb_alloc_dma_scratch(usb_dma_scratch_t *scratch) {
    if (!scratch) return -1;
    scratch->raw = NULL;
    scratch->page = NULL;

    // Allocate enough headroom to carve out one 4KB aligned DMA-safe page.
    uint8_t *raw = (uint8_t *)malloc(8192);
    if (!raw) return -1;

    uintptr_t aligned = ((uintptr_t)raw + 4095ULL) & ~4095ULL;
    if (aligned + 4096ULL > (uintptr_t)raw + 8192ULL) {
        free(raw);
        return -1;
    }

    scratch->raw = raw;
    scratch->page = (uint8_t *)aligned;
    memset(scratch->page, 0, 4096);
    return 0;
}

static void usb_free_dma_scratch(usb_dma_scratch_t *scratch) {
    if (!scratch) return;
    if (scratch->raw) free(scratch->raw);
    scratch->raw = NULL;
    scratch->page = NULL;
}

static int usb_control_transfer_retry(usb_hcd_t *hcd, usb_device_t *dev,
                                      usb_setup_packet_t *setup, void *data, uint16_t length,
                                      int retries, uint64_t retry_delay_ms) {
    if (retries < 1) retries = 1;
    int ret = -1;
    for (int i = 0; i < retries; i++) {
        ret = hcd->control_transfer(hcd, dev, setup, data, length);
        if (ret >= 0) return ret;
        if (i + 1 < retries && retry_delay_ms) sleep(retry_delay_ms);
    }
    return ret;
}

static uint8_t usb_next_probe_address = 1;

static int usb_allocate_probe_address(uint8_t *out_addr) {
    if (!out_addr) return -1;
    if (usb_next_probe_address == 0 || usb_next_probe_address > 127) {
        return -1;
    }
    *out_addr = usb_next_probe_address++;
    return 0;
}

// ============================================================================
// USB keyboard initialisation — verifies device is actually a boot keyboard
// before committing any resources.
// ============================================================================
void init_usb_keyboard(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id) {
    if (!hcd || kbd_total >= MAX_USB_KEYBOARDS) return;

    // Check if already registered on this HCD + port
    for (int i = 0; i < kbd_total; i++) {
        if (kbd_list[i].hcd == hcd && kbd_list[i].dev &&
            kbd_list[i].dev->port_id == port_id) return;
    }

    if (usb_device_count >= USB_MAX_DEVICES) {
        printf("USB: Max devices reached, cannot probe port %d.\n", port_id);
        return;
    }

    // xHCI handles its own enumeration in initialize_device_xhci()
    if (strcmp(hcd->name, "xHCI") == 0) {
        usb_device_t *dev = &usb_devices[usb_device_count];
        dev->address      = usb_device_count + 1;
        dev->speed        = speed;
        dev->port_id      = port_id;
        dev->interrupt_toggle = 0;
        usb_device_count++;

        kbd_list[kbd_total].dev            = dev;
        kbd_list[kbd_total].hcd            = hcd;
        kbd_list[kbd_total].report_buf     = NULL;
        kbd_list[kbd_total].report_buf_next = NULL;
        memset(kbd_list[kbd_total].prev_report, 0, 8);
        kbd_list[kbd_total].repeat_key     = 0;
        kbd_list[kbd_total].repeat_timer   = 0;
        kbd_total++;
        return;
    }

    // ----------------------------------------------------------------
    // Non-xHCI path: enumerate at address 0 and verify device class
    // before fully committing.
    // ----------------------------------------------------------------

    // Allocate report ping-pong buffers upfront so we have a single
    // cleanup path if anything fails during enumeration.
    uint8_t *rbuf = malloc(8);
    uint8_t *rbuf2 = malloc(8);
    if (!rbuf || !rbuf2) {
        printf("USB: Failed to allocate report buffers for port %d.\n", port_id);
        if (rbuf)  free(rbuf);
        if (rbuf2) free(rbuf2);
        return;
    }
    memset(rbuf,  0, 8);
    memset(rbuf2, 0, 8);

    usb_dma_scratch_t dma = {0};
    if (usb_alloc_dma_scratch(&dma) < 0) {
        printf("USB: Failed to allocate DMA scratch for port %d.\n", port_id);
        free(rbuf);
        free(rbuf2);
        return;
    }

    // Keep all control transfer payloads in one DMA-safe page so EHCI/OHCI
    // never see buffers that straddle a 4KB boundary.
    usb_setup_packet_t *setup = (usb_setup_packet_t *)(dma.page + 0);
    uint8_t *desc_buf = dma.page + 64;
    uint8_t *cfg_buf = dma.page + 128;

    // Temporarily use a local device slot at address 0 for probing.
    // We'll commit it to the global registry only if this is a keyboard.
    usb_device_t *dev = &usb_devices[usb_device_count];
    uint8_t new_address = 0;
    if (usb_allocate_probe_address(&new_address) < 0) {
        printf("USB: No free device addresses left for port %d.\n", port_id);
        goto fail_probe;
    }
    dev->address          = 0;
    dev->speed            = speed;
    dev->max_packet_size  = (speed == USB_SPEED_HIGH) ? 64 : 8;
    dev->port_id          = port_id;
    dev->interrupt_toggle = 0;
    dev->hcd_data         = NULL;
    memset(desc_buf, 0, 64);

    // ---- Step 1: GET_DESCRIPTOR (Device, first 8 bytes at address 0) ----
    setup->bmRequestType = USB_REQTYPE_DIR_IN  | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup->wValue        = (USB_DESC_DEVICE << 8) | 0;
    setup->wIndex        = 0;
    setup->wLength       = 8;
    if (usb_control_transfer_retry(hcd, dev, setup, desc_buf, 8, 4, 10) < 0) {
        printf("USB: GET_DESCRIPTOR(device,8) failed on port %d.\n", port_id);
        goto fail_probe;
    }
    uint8_t ep0_mps = desc_buf[7];
    if (speed == USB_SPEED_HIGH) ep0_mps = 64;
    if (ep0_mps != 8 && ep0_mps != 16 && ep0_mps != 32 && ep0_mps != 64) {
        ep0_mps = (speed == USB_SPEED_HIGH) ? 64 : 8;
    }
    dev->max_packet_size = ep0_mps;

    // ---- Step 2: SET_ADDRESS ----
    setup->bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_SET_ADDRESS;
    setup->wValue        = new_address;
    setup->wIndex        = 0;
    setup->wLength       = 0;
    if (usb_control_transfer_retry(hcd, dev, setup, NULL, 0, 3, 10) < 0) {
        printf("USB: SET_ADDRESS failed on port %d.\n", port_id);
        goto fail_probe;
    }
    dev->address = new_address;
    // USB 2.0 requires at least 2ms before using the new address.
    sleep(20);

    // ---- Step 3: GET_DESCRIPTOR (Device Descriptor, 18 bytes) ----
    memset(desc_buf, 0, 64);

    setup->bmRequestType = USB_REQTYPE_DIR_IN  | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup->wValue        = (USB_DESC_DEVICE << 8) | 0;
    setup->wIndex        = 0;
    setup->wLength       = 18;
    if (usb_control_transfer_retry(hcd, dev, setup, desc_buf, 18, 5, 20) < 0) {
        printf("USB: GET_DESCRIPTOR(device) failed on port %d.\n", port_id);
        goto fail_probe;
    }

    usb_device_descriptor_t *ddev = (usb_device_descriptor_t *)desc_buf;
    dev->vendor_id = ddev->idVendor;
    dev->product_id = ddev->idProduct;

    // Reject devices whose top-level class is not interface-defined (0x00) or HID (0x03).
    // Mass storage is 0x08, audio is 0x01, CDC is 0x02, hub is 0x09, etc.
    if (ddev->bDeviceClass != 0x00 && ddev->bDeviceClass != USB_HID_CLASS) {
        printf("USB: Port %d: Not a HID device (class=0x%02X), ignoring.\n",
               port_id, ddev->bDeviceClass);
        goto fail_probe;
    }

    // ---- Step 4a: GET_DESCRIPTOR (Configuration header, 9 bytes) ----
    memset(cfg_buf, 0, 64);
    setup->bmRequestType = USB_REQTYPE_DIR_IN  | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup->wValue        = (USB_DESC_CONFIGURATION << 8) | 0;
    setup->wIndex        = 0;
    setup->wLength       = sizeof(usb_config_descriptor_t);
    if (usb_control_transfer_retry(hcd, dev, setup, cfg_buf, sizeof(usb_config_descriptor_t), 4, 10) < 0) {
        printf("USB: GET_DESCRIPTOR(config) failed on port %d.\n", port_id);
        goto fail_probe;
    }

    // ---- Step 4b: GET_DESCRIPTOR (Configuration body, clamped to 64 bytes) ----
    usb_config_descriptor_t *dcfg = (usb_config_descriptor_t *)cfg_buf;
    uint16_t cfg_read_len = dcfg->wTotalLength;
    if (cfg_read_len < sizeof(usb_config_descriptor_t)) {
        cfg_read_len = sizeof(usb_config_descriptor_t);
    }
    if (cfg_read_len > 64) cfg_read_len = 64;

    if (cfg_read_len > sizeof(usb_config_descriptor_t)) {
        memset(cfg_buf, 0, 64);
        setup->wLength = cfg_read_len;
        if (usb_control_transfer_retry(hcd, dev, setup, cfg_buf, cfg_read_len, 4, 10) < 0) {
            printf("USB: GET_DESCRIPTOR(config body) failed on port %d.\n", port_id);
            goto fail_probe;
        }
    }

    dcfg = (usb_config_descriptor_t *)cfg_buf;
    uint16_t total_len = dcfg->wTotalLength;
    if (total_len > cfg_read_len) total_len = cfg_read_len;

    // Walk descriptors looking for a boot-keyboard interface
    if (!usb_find_boot_keyboard_interface(cfg_buf, total_len)) {
        printf("USB: Port %d: No boot keyboard interface found (class check failed).\n",
               port_id);
        goto fail_probe;
    }

    // ---- This is confirmed to be a boot keyboard — commit resources ----
    usb_device_count++;

    kbd_list[kbd_total].dev             = dev;
    kbd_list[kbd_total].hcd             = hcd;
    kbd_list[kbd_total].report_buf      = rbuf;
    kbd_list[kbd_total].report_buf_next = rbuf2;
    memset(kbd_list[kbd_total].prev_report, 0, 8);
    kbd_list[kbd_total].repeat_key      = 0;
    kbd_list[kbd_total].repeat_timer    = 0;
    kbd_total++;

    // ---- Step 5: SET_CONFIGURATION 1 ----
    setup->bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup->bRequest      = USB_REQ_SET_CONFIGURATION;
    setup->wValue        = dcfg->bConfigurationValue;
    setup->wIndex        = 0;
    setup->wLength       = 0;
    hcd->control_transfer(hcd, dev, setup, NULL, 0);

    // ---- Step 6: SET_PROTOCOL 0 (Boot Protocol) ----
    setup->bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE;
    setup->bRequest      = USB_REQ_SET_PROTOCOL;
    setup->wValue        = 0; // 0 = Boot Protocol
    setup->wIndex        = 0; // Interface 0
    setup->wLength       = 0;
    hcd->control_transfer(hcd, dev, setup, NULL, 0);

    printf("%s: Boot keyboard confirmed on port %d (address %d, vid=%04X pid=%04X).\n",
           hcd->name, port_id, dev->address,
           ddev->idVendor, ddev->idProduct);
    usb_free_dma_scratch(&dma);
    return;

fail_probe:
    free(rbuf);
    free(rbuf2);
    usb_free_dma_scratch(&dma);
}
