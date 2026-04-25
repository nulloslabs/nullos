#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>
#include <io/uhci.h>
#include <io/usb.h>
#include <io/pci.h>
#include <io/io.h>
#include <io/terminal.h>
#include <io/usb_keyboard.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <io/hpet.h>
#include <main/acpi.h>

static uhci_controller_t uhci_controllers[MAX_UHCI_CONTROLLERS];
static int uhci_count = 0;

static uhci_td_t *uhci_alloc_td(void) {
    uhci_td_t *td = (uhci_td_t *)malloc(sizeof(uhci_td_t) + 16);
    td = (uhci_td_t *)(((uint64_t)td + 15) & ~15ULL);
    memset(td, 0, sizeof(uhci_td_t));
    td->link_ptr = UHCI_PTR_TERMINATE;
    return td;
}

static uhci_qh_t *uhci_alloc_qh(void) {
    uhci_qh_t *qh = (uhci_qh_t *)malloc(sizeof(uhci_qh_t) + 16);
    qh = (uhci_qh_t *)(((uint64_t)qh + 15) & ~15ULL);
    memset(qh, 0, sizeof(uhci_qh_t));
    qh->head_link_ptr = UHCI_PTR_TERMINATE;
    qh->element_link_ptr = UHCI_PTR_TERMINATE;
    return qh;
}

// Check TD completion without blocking
static int uhci_check_td(uhci_td_t *td) {
    uint32_t status = *(volatile uint32_t*)&td->status;
    if (status & UHCI_TD_ACTIVE) {
        return -1; // still active
    }
    if (status & (0x7E << 16)) { // error bits: Stalled, DataBuf, Babble, NAK, CRC/Timeout, Bitstuff (bits 22:17)
        return -2; // error
    }
    return 0; // success
}

// ============================================================================
// Transfer stubs
// ============================================================================
static int uhci_control_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                                  usb_setup_packet_t *setup, void *data, uint16_t length) {
    uhci_controller_t *ctrl = (uhci_controller_t *)hcd->hcd_data;
    uint8_t addr = dev->address;
    int low_speed = (dev->speed == USB_SPEED_LOW);

    uhci_td_t *td_setup = uhci_alloc_td();
    td_setup->token = (7 << 21) | (0 << 19) | (0 << 15) | (addr << 8) | UHCI_PID_SETUP;
    td_setup->buffer_ptr = (uint32_t)virt_to_phys(setup);
    td_setup->status = UHCI_TD_ACTIVE | (low_speed ? UHCI_TD_LS : 0) | (3 << 27);
    td_setup->link_ptr = UHCI_PTR_TERMINATE;

    uhci_td_t *td_data = NULL;
    if (length > 0) {
        td_data = uhci_alloc_td();
        td_data->token = ((length - 1) << 21) | (1 << 19) | (0 << 15) | (addr << 8) | UHCI_PID_IN;
        td_data->buffer_ptr = (uint32_t)virt_to_phys(data);
        td_data->status = UHCI_TD_ACTIVE | (low_speed ? UHCI_TD_LS : 0) | (3 << 27);
        td_data->link_ptr = UHCI_PTR_TERMINATE;
        td_setup->link_ptr = (uint32_t)virt_to_phys(td_data);
    }

    uhci_td_t *td_status = uhci_alloc_td();
    uint8_t status_pid = (length > 0) ? UHCI_PID_OUT : UHCI_PID_IN;
    td_status->token = (0 << 21) | (1 << 19) | (0 << 15) | (addr << 8) | status_pid;  // MaxLen=0 for status
    td_status->buffer_ptr = 0;
    td_status->status = UHCI_TD_ACTIVE | (low_speed ? UHCI_TD_LS : 0) | (3 << 27);
    td_status->link_ptr = UHCI_PTR_TERMINATE;

    if (td_data) {
        td_data->link_ptr = (uint32_t)virt_to_phys(td_status);
	    } else {
        td_setup->link_ptr = (uint32_t)virt_to_phys(td_status);
    }

    ctrl->qh->element_link_ptr = (uint32_t)virt_to_phys(td_setup);

    // Block until the status TD completes (control transfers only happen during init)
    int ret = -2;
    for (int i = 0; i < 10000; i++) {
        ret = uhci_check_td(td_status);
        if (ret != -1) break;
        sleep(1);
    }

    ctrl->qh->element_link_ptr = UHCI_PTR_TERMINATE;
    return ret;
}

static int uhci_interrupt_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                                    uint8_t endpoint, void *data, uint16_t length) {
    uhci_controller_t *ctrl = (uhci_controller_t *)hcd->hcd_data;
    uint8_t addr = dev->address;
    int low_speed = (dev->speed == USB_SPEED_LOW);

    // If there's already a pending transfer, return busy
    if (ctrl->pending_dev != NULL) {
        return -1;
    }

    uhci_td_t *td = ctrl->pending_td;
    td->buffer_ptr = (uint32_t)virt_to_phys(data);
    td->token = ((length - 1) << 21) | (dev->interrupt_toggle << 19) | (endpoint << 15) | (addr << 8) | UHCI_PID_IN;
    td->status = UHCI_TD_ACTIVE | (low_speed ? UHCI_TD_LS : 0) | (3 << 27);
    td->link_ptr = UHCI_PTR_TERMINATE;

    // Prepend to interrupt QH chain
    uint32_t old_head = ctrl->intr_qh->element_link_ptr;
    td->link_ptr = old_head;
    ctrl->intr_qh->element_link_ptr = (uint32_t)virt_to_phys(td);

    // Save pending state
    ctrl->pending_dev = dev;
    ctrl->pending_buf = (uint8_t*)data;
    ctrl->pending_td = td;
    ctrl->pending_len = length;

    return 0; // submitted
}

static int uhci_bulk_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                               uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)endpoint; (void)data; (void)length;
    return -1;
}

// ============================================================================
// Hot-plug: poll ports for connect/disconnect changes
// ============================================================================
void poll_uhci_ports(void) {
    for (int c = 0; c < uhci_count; c++) {
        uhci_controller_t *ctrl = &uhci_controllers[c];
        if (!ctrl->initialized) continue;

        uint16_t io_base = ctrl->io_base;
        uint16_t port_regs[] = { UHCI_PORTSC1, UHCI_PORTSC2 };

        for (int i = 0; i < 2; i++) {
            uint16_t status = inw(io_base + port_regs[i]);

            if (status & UHCI_PORT_CSC) {
                outw(io_base + port_regs[i], status | UHCI_PORT_CSC);

                if (status & UHCI_PORT_CCS) {
                    int ls = (status & UHCI_PORT_LSDA) ? 1 : 0;
                    printf("UHCI[%d]: Port %d: Device connected (speed=%s).\n", c, i, ls ? "LOW" : "FULL");

                    // Clear pending state on new connection
                    ctrl->pending_dev = NULL;
                    ctrl->pending_buf = NULL;
                    if (ctrl->intr_qh) ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;

                    // Reset port
                    outw(io_base + port_regs[i], UHCI_PORT_RESET);
                    sleep(50);
                    outw(io_base + port_regs[i], 0);
                    sleep(10);
                    // Enable port
                    outw(io_base + port_regs[i], UHCI_PORT_PED);
                    sleep(10);

                    register_usb_hcd(&ctrl->hcd);
                    init_usb_keyboard(&ctrl->hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
                } else {
                    // Device disconnected
                    printf("UHCI[%d]: Port %d: Device disconnected.\n", c, i);
                    // Clear pending on disconnect and remove keyboard from list
                    for (int k = 0; k < kbd_total; k++) {
                        if (kbd_list[k].hcd == &ctrl->hcd &&
                            kbd_list[k].dev && kbd_list[k].dev->port_id == i) {
                            if (kbd_list[k].dev == ctrl->pending_dev) {
                                ctrl->pending_dev = NULL;
                                ctrl->pending_buf = NULL;
                            }
                            memset(&kbd_list[k], 0, sizeof(kbd_entry_t));
                            kbd_total--;
                            if (k < kbd_total) {
                                memmove(&kbd_list[k], &kbd_list[k + 1],
                                        (kbd_total - k) * sizeof(kbd_entry_t));
                            }
                            break;
                        }
                    }
                    ctrl->pending_dev = NULL;
                    ctrl->pending_buf = NULL;
                    if (ctrl->intr_qh) ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;
                }
            }
        }

        // Keyboard polling: check pending and re-queue
        if (ctrl->pending_dev && ctrl->pending_td) {
            int ret = uhci_check_td(ctrl->pending_td);
            if (ret == 0) {
                usb_device_t *dev = ctrl->pending_dev;
                uint8_t *buf = ctrl->pending_buf;

                if (buf && dev) {
                    dev->interrupt_toggle ^= 1;
                    int ki = kbd_find_index(dev);
                    if (ki >= 0 && ki < kbd_total) {
                        usb_keyboard_process_report(buf, ki);
                        uint8_t *temp = kbd_list[ki].report_buf;
                        kbd_list[ki].report_buf = kbd_list[ki].report_buf_next;
                        kbd_list[ki].report_buf_next = temp;
                    }
                }
                ctrl->pending_dev = NULL;
                ctrl->pending_buf = NULL;
                ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;
            } else if (ret == -2) {
                ctrl->pending_dev = NULL;
                ctrl->pending_buf = NULL;
                ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;
            }
        }
        // Arm next UHCI keyboard on this controller if none pending
        if (!ctrl->pending_dev) {
            for (int k = 0; k < kbd_total; k++) {
                usb_hcd_t *hcd = kbd_list[k].hcd;
                usb_device_t *dev = kbd_list[k].dev;
                uint8_t *buf = kbd_list[k].report_buf_next;
                if (hcd == &ctrl->hcd && dev && buf) {
                    int ret = ctrl->hcd.interrupt_transfer(&ctrl->hcd, dev, 1, buf, 8);
                    if (ret == 0) break;
                }
            }
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================
void init_uhci(pci_device_t *dev) {
    if (uhci_count >= MAX_UHCI_CONTROLLERS) return;

    uint32_t bar4 = read_pci(dev->bus, dev->dev, dev->func, 0x20);
    uint64_t io_base = (uint64_t)(bar4 & ~0x03);

    if (io_base == 0) {
        printf("UHCI: No I/O base found.\n");
        return;
    }

    set_pci_d0(dev);

    // Prevent initializing the same I/O base twice
    for (int i = 0; i < uhci_count; i++) {
        if (uhci_controllers[i].io_base == (uint16_t)io_base) return;
    }

    uhci_controller_t *ctrl = &uhci_controllers[uhci_count];
    memset(ctrl, 0, sizeof(uhci_controller_t));

    ctrl->io_base = (uint16_t)io_base;

    // ---- ACPI _CRS validation of I/O base ----
    acpi_resource_list_t res_list = {0};
    if (get_acpi_usb_resources(dev->bus, dev->dev, dev->func, &res_list)) {
        ctrl->acpi_validated = 1;
        printf("UHCI[%d]: ACPI _CRS confirmed %d resources.\n", uhci_count, res_list.count);
        for (int r = 0; r < res_list.count; r++) {
            acpi_resource_t *res = &res_list.resources[r];
            if (!res->valid) continue;
            if (res->type == ACPI_RES_IO) {
                printf("UHCI[%d]:   IO range 0x%04X-0x%04X\n", uhci_count,
                       (uint16_t)res->base, (uint16_t)(res->base + res->length - 1));
                /* Validate that PCI BAR I/O base falls within ACPI range */
                uint16_t bar_lo = (uint16_t)io_base;
                uint16_t acpi_lo = (uint16_t)res->base;
                uint16_t acpi_hi = (uint16_t)(res->base + res->length - 1);
                if (bar_lo >= acpi_lo && bar_lo <= acpi_hi) {
                    printf("UHCI[%d]:   PCI BAR I/O 0x%04X is within ACPI range -- VALID.\n",
                           uhci_count, bar_lo);
                } else {
                    printf("UHCI[%d]:   WARNING: PCI BAR I/O 0x%04X is OUTSIDE ACPI range!\n",
                           uhci_count, bar_lo);
                }
            }
            else if (res->type == ACPI_RES_IRQ) {
                printf("UHCI[%d]:   IRQ %u\n", uhci_count, res->irq);
            }
        }
        /* Use ACPI port count if available (UHCI typically has 2 ports per controller) */
        /* UHCI always has 2 ports, but ACPI can tell us if the controller is actually present */
        ctrl->num_ports = 2; /* UHCI fixed at 2 ports */
    } else {
        printf("UHCI[%d]: No ACPI _CRS match -- using PCI BAR only.\n", uhci_count);
        ctrl->num_ports = 2;
    }

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 0) | (1 << 2);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd);

    // UHCI BIOS Handoff: disable USB Legacy Support (SMI generation)
    uint32_t legsup = read_pci(dev->bus, dev->dev, dev->func, 0xC0);
    write_pci(dev->bus, dev->dev, dev->func, 0xC0, (legsup & 0xFFFF0000) | 0x8F00);

    outw(io_base + UHCI_USBCMD, UHCI_CMD_GRESET);
    sleep(50);
    outw(io_base + UHCI_USBCMD, 0);
    sleep(10);

    outw(io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 1000; i++) {
        if (!(inw(io_base + UHCI_USBCMD) & UHCI_CMD_HCRESET)) break;
        sleep_us(100);
    }

    ctrl->frame_list = (uint32_t*)malloc(UHCI_FRAME_LIST_SIZE * sizeof(uint32_t) + 4096);
    ctrl->frame_list = (uint32_t*)(((uint64_t)ctrl->frame_list + 4095) & ~4095ULL);
    ctrl->frame_list_phys = virt_to_phys(ctrl->frame_list);

    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) {
        ctrl->frame_list[i] = UHCI_PTR_TERMINATE;
    }

    outl(io_base + UHCI_FLBASEADD, (uint32_t)ctrl->frame_list_phys);
    outw(io_base + UHCI_FRNUM, 0);
    outw(io_base + UHCI_USBSTS, 0xFFFF);

    ctrl->qh = uhci_alloc_qh();
    ctrl->intr_qh = uhci_alloc_qh();
    ctrl->intr_qh->head_link_ptr = (uint32_t)virt_to_phys(ctrl->qh) | UHCI_PTR_QH;

    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) {
        ctrl->frame_list[i] = (uint32_t)virt_to_phys(ctrl->intr_qh) | UHCI_PTR_QH;
    }

    outb(io_base + UHCI_SOFMOD, 64);
    outw(io_base + UHCI_USBINTR, 0x0000);
    outw(io_base + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);

    ctrl->hcd.name = "UHCI";
    ctrl->hcd.control_transfer = uhci_control_transfer;
    ctrl->hcd.interrupt_transfer = uhci_interrupt_transfer;
    ctrl->hcd.bulk_transfer = uhci_bulk_transfer;
    ctrl->hcd.hcd_data = ctrl;
    ctrl->initialized = 1;
    ctrl->pending_dev = NULL;
    ctrl->pending_buf = NULL;
    ctrl->pending_td = uhci_alloc_td();

    printf("UHCI[%d]: Initialized (io=0x%04x).\n", uhci_count, (uint16_t)io_base);
    uhci_count++;

    // Initial port scan - detect already-connected devices
    uint16_t port_regs[] = { UHCI_PORTSC1, UHCI_PORTSC2 };
    for (int i = 0; i < 2; i++) {
        uint16_t status = inw(io_base + port_regs[i]);

        if (status & UHCI_PORT_CCS) {
            int ls = (status & UHCI_PORT_LSDA) ? 1 : 0;

            outw(io_base + port_regs[i], UHCI_PORT_RESET);
            sleep(50);
            outw(io_base + port_regs[i], 0);
            sleep(10);
            outw(io_base + port_regs[i], UHCI_PORT_PED);
            sleep(10);

            status = inw(io_base + port_regs[i]);
            outw(io_base + port_regs[i], status | UHCI_PORT_CSC | UHCI_PORT_PEDC);

            register_usb_hcd(&ctrl->hcd);
            init_usb_keyboard(&ctrl->hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
        }
    }
}
// ============================================================================
// Companion query/notification for EHCI handoff
// ============================================================================
bool is_uhci_ready(void) {
    return uhci_count > 0;
}

// Scan the companion controller at ctrl_idx for newly handed-off devices.
// ctrl_idx  < 0 => all initialized controllers.
// port_hint < 0 => all ports on selected controller(s), otherwise only that port.
void rescan_uhci_ports(int ctrl_idx, int port_hint) {
    int start = (ctrl_idx < 0) ? 0 : ctrl_idx;
    int end   = (ctrl_idx < 0) ? uhci_count : (ctrl_idx + 1);

    // Clamp to valid range
    if (start >= uhci_count) start = 0;
    if (end   > uhci_count)  end   = uhci_count;

    for (int c = start; c < end; c++) {
        uhci_controller_t *ctrl = &uhci_controllers[c];
        if (!ctrl->initialized) continue;

        uint16_t io_base = ctrl->io_base;
        uint16_t port_regs[] = { UHCI_PORTSC1, UHCI_PORTSC2 };
        uint16_t num_ports = ctrl->num_ports ? ctrl->num_ports : 2;

        int port_start = 0;
        int port_end = (num_ports < 2) ? num_ports : 2;
        if (port_hint >= 0 && port_hint < port_end) {
            port_start = port_hint;
            port_end = port_hint + 1;
        }

        for (int i = port_start; i < port_end; i++) {
            uint16_t status = inw(io_base + port_regs[i]);
            printf("UHCI[%d]: Port %d: Initial status=0x%04X (CCS=%d, PED=%d, CSC=%d)\n",
                   c, i, status, !!(status & UHCI_PORT_CCS), !!(status & UHCI_PORT_PED), !!(status & UHCI_PORT_CSC));

            // Skip completely invalid ports (0x0000 = no port register / invalid I/O)
            if (status == 0x0000) {
                printf("UHCI[%d]: Port %d: Invalid/absent port register (0x0000), skipping.\n", c, i);
                continue;
            }

            // Clear any stale CSC from the ownership transition
            if (status & UHCI_PORT_CSC) {
                outw(io_base + port_regs[i], status | UHCI_PORT_CSC);
                status = inw(io_base + port_regs[i]);
                printf("UHCI[%d]: Port %d: Cleared CSC, new status=0x%04X\n", c, i, status);
            }

            // Skip ports that are already enabled (already enumerated)
            if (status & UHCI_PORT_PED) {
                printf("UHCI[%d]: Port %d: Skipping (already enabled).\n", c, i);
                continue;
            }

            // Wait for CCS to appear — the routing matrix may take a few ms
            if (!(status & UHCI_PORT_CCS)) {
                // Quick check: give the routing matrix just 5ms to settle
                sleep(5);
                status = inw(io_base + port_regs[i]);
                if (status & UHCI_PORT_CSC) {
                    outw(io_base + port_regs[i], status | UHCI_PORT_CSC);
                    status = inw(io_base + port_regs[i]);
                }
                if (!(status & UHCI_PORT_CCS)) {
                    printf("UHCI[%d]: Port %d: No device, skipping.\n", c, i);
                    continue;
                }
            }

            // 100ms debounce per USB 2.0 §9.1.2
            sleep(100);

            int ls = (status & UHCI_PORT_LSDA) ? 1 : 0;
            printf("UHCI[%d]: Port %d: Companion handoff device (speed=%s).\n",
                   c, i, ls ? "LOW" : "FULL");

            ctrl->pending_dev = NULL;
            ctrl->pending_buf = NULL;
            if (ctrl->intr_qh) ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;

            outw(io_base + port_regs[i], UHCI_PORT_RESET);
            sleep(50);
            outw(io_base + port_regs[i], 0);
            sleep(10);
            outw(io_base + port_regs[i], UHCI_PORT_PED);
            sleep(10);

            status = inw(io_base + port_regs[i]);
            outw(io_base + port_regs[i], status | UHCI_PORT_CSC | UHCI_PORT_PEDC);

            if (!(status & UHCI_PORT_PED)) {
                printf("UHCI[%d]: Port %d: Port not enabled after handoff reset.\n", c, i);
                continue;
            }

            register_usb_hcd(&ctrl->hcd);
            init_usb_keyboard(&ctrl->hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
            printf("UHCI[%d]: Port %d: Handoff device enumerated.\n", c, i);
        }
    }
}
