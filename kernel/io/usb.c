#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/usb.h>
#include <io/terminal.h>
#include <io/uhci.h>
#include <io/ohci.h>
#include <io/ehci.h>
#include <io/xhci.h>
#include <io/usb_keyboard.h>
#include <io/ps2_keyboard.h>
#include <main/halt.h>
#include <main/mp.h>

usb_device_t usb_devices[USB_MAX_DEVICES];
int usb_device_count = 0;
usb_hcd_t *usb_active_hcd = NULL;

void register_usb_hcd(usb_hcd_t *hcd) {
    if (!usb_active_hcd) {
        usb_active_hcd = hcd;
    }
}

void poll_usb_hcds(void) {
    if (system_halted) return;
    // Only poll USB from BSP (CPU 0) to avoid multi-CPU race conditions
    // and to keep poll rate predictable for key repeat timing
    if (get_cpu_index() != 0) return;
    static int poll_counter = 0;
    // Poll every 1 tick (4ms at 250Hz). EHCI/UHCI/OHCI/xHCI all use
    // polling (no IRQ for keyboard transfers), so tighter interval = lower latency.
    // PS/2 is IRQ-driven (<1ms) and will always feel snappier.
    if (++poll_counter >= 1) {
        poll_counter = 0;
        poll_uhci_ports();
        poll_ohci_ports();
        poll_ehci_ports();
        poll_xhci_ports();
        poll_usb_keyboard();
        poll_ps2_keyboard();
    }
}
