#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/usb.h>
#include <io/uhci.h>
#include <io/usb_keyboard.h>
#include <io/ps2_keyboard.h>
#include <main/halt.h>
#include <main/mp.h>
#include <main/string.h>
#include <mm/mm.h>

usb_device_t *usb_devices_head = NULL;
usb_hcd_t *usb_active_hcd = NULL;

static uint8_t usb_address_pool[128] = {0};

void register_usb_hcd(usb_hcd_t *hcd) {
    if (!usb_active_hcd) { usb_active_hcd = hcd; } }

int usb_allocate_address(int hint) {
    if (hint > 0 && hint < 128) { if (usb_address_pool[hint] == 0) { usb_address_pool[hint] = 1; return hint; } }
    for (int i = 1; i < 128; i++) { if (usb_address_pool[i] == 0) { usb_address_pool[i] = 1; return i; } }
    return -1;
}

void usb_release_address(uint8_t addr) {
    if (addr > 0 && addr < 128) { usb_address_pool[addr] = 0; } }

usb_device_t* usb_allocate_device(void) {
    usb_device_t *dev = malloc(sizeof(usb_device_t));
    if (!dev) return NULL;
    memset(dev, 0, sizeof(usb_device_t));
    
    // Add to linked list
    dev->next = usb_devices_head;
    usb_devices_head = dev;
    
    return dev;
}

void unregister_usb_device(usb_device_t *dev) {
    if (!dev) return;
    if (dev->address > 0) { usb_release_address(dev->address); }
    
    // Remove from linked list
    if (usb_devices_head == dev) {
        usb_devices_head = dev->next;
    } else {
        usb_device_t *prev = usb_devices_head;
        while (prev && prev->next != dev) prev = prev->next;
        if (prev) prev->next = dev->next;
    }
    
    free(dev);
}

void poll_usb_hcds(void) {
    if (system_halted) return;
    if (get_cpu_index() != 0) return;
    static int poll_counter = 0;
    if (++poll_counter >= 1) {
        poll_counter = 0;
        poll_uhci_ports();
        poll_usb_keyboard();
    }
}
