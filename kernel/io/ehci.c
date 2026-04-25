#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/ehci.h>
#include <io/usb.h>
#include <io/pci.h>
#include <io/io.h>
#include <io/terminal.h>
#include <io/usb_keyboard.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <main/halt.h>
#include <io/ohci.h>
#include <io/uhci.h>
#include <io/hpet.h>
#include <main/acpi.h>

#define MAX_EHCI_CONTROLLERS 8
static ehci_controller_t ehci_controllers[MAX_EHCI_CONTROLLERS];
static int ehci_controller_count = 0;
static uint64_t ehci_initialized_bars[MAX_EHCI_CONTROLLERS];

// ============================================================================
// MMIO helpers
// ============================================================================
static uint32_t ehci_cap_read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}

static uint8_t ehci_cap_read8(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint8_t*)(base + offset);
}

static uint32_t ehci_op_read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}

static void ehci_op_write32(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(base + offset) = val;
}

// Preserve read-write bits, mask out W1C bits to avoid accidentally clearing them
static uint32_t ehci_portsc_preserve(uint32_t portsc) {
    return portsc & ~(EHCI_PORT_CSC | EHCI_PORT_PEDC | EHCI_PORT_OCC);
}

// Read PORTSC with bounded settle retries.
// Real hardware may transiently return 0x00000000 while port power/routing settles.
static uint32_t ehci_read_portsc_with_settle(volatile uint8_t *op, uint32_t port_reg,
                                             int ppc, int attempts, uint64_t delay_ms) {
    uint32_t portsc = 0;
    for (int i = 0; i < attempts; i++) {
        portsc = ehci_op_read32(op, port_reg);
        if (portsc != 0x00000000) return portsc;
        if (ppc) {
            // In PPC mode, explicitly keep PP asserted while waiting for settle.
            ehci_op_write32(op, port_reg, EHCI_PORT_PP);
        }
        if (delay_ms) sleep(delay_ms);
    }
    return portsc;
}

/* Notify the correct OHCI/UHCI companion controller after EHCI sets PORT_OWNER.
 * port_idx: the EHCI port that was handed off.
 * Uses HCSPARAMS N_PCC to compute which companion controller owns that port. */
static void notify_ehci_companion(ehci_controller_t *ctrl, int port_idx) {
    // Give the routing matrix time to settle on real hardware.
    // 10ms was too short for ICH silicon; 25ms is safe.
    sleep(25);

    if (is_ohci_ready()) {
        // Target only the handed-off root port to avoid resetting/scanning all
        // OHCI ports on every EHCI handoff event.
        ohci_rescan_ports(port_idx);
    } else if (is_uhci_ready()) {
        // Compute which UHCI companion controller owns this EHCI port.
        // EHCI HCSPARAMS N_PCC tells us how many EHCI ports each UHCI owns.
        // companion_idx = port_idx / n_pcc, matching PCI enumeration order.
        int n_pcc = ctrl->n_pcc;
        int companion_idx = (n_pcc > 0) ? (port_idx / n_pcc) : 0;
        int companion_port = (n_pcc > 0) ? (port_idx % n_pcc) : 0;
        rescan_uhci_ports(companion_idx, companion_port);
    } else {
        printf("EHCI: No OHCI/UHCI companion controller available for handoff (port %d)!\n", port_idx);
    }
}

/* Busy-wait for qTD completion. */
static int wait_ehci_qtd(ehci_qtd_t *qtd, int max_iters, int relax_per_iter) {
    for (int i = 0; i < max_iters; i++) {
        uint32_t token = *(volatile uint32_t *)&qtd->token;
        if (!(token & (1 << 7))) { // Status Active bit
            if (token & 0x7C) return -1; // Status Error bits
            return 0;
        }
        for (int r = 0; r < relax_per_iter; r++) {
            pause();
        }
    }
    return -2;
}

static ehci_qtd_t *alloc_ehci_qtd(void) {
    ehci_qtd_t *qtd = (ehci_qtd_t *)malloc(sizeof(ehci_qtd_t) + 32);
    qtd = (ehci_qtd_t *)(((uint64_t)qtd + 31) & ~31ULL);
    memset(qtd, 0, sizeof(ehci_qtd_t));
    qtd->next_qtd = 0x01; // Terminate
    qtd->alt_next_qtd = 0x01;
    return qtd;
}

static int ehci_fill_qtd_buffers(ehci_qtd_t *qtd, void *buf, uint16_t len) {
    if (!qtd) return -1;
    memset(qtd->buffer, 0, sizeof(qtd->buffer));
    if (!buf || len == 0) return 0;

    uint64_t phys0 = virt_to_phys(buf);
    if (phys0 > 0xFFFFFFFFULL) return -1;
    qtd->buffer[0] = (uint32_t)phys0;

    // qTD has up to 5 page pointers. buffer[0] keeps the original byte offset;
    // subsequent pointers hold page bases for continuation pages.
    uint64_t offset0 = phys0 & 0xFFFULL;
    if (len <= (uint16_t)(4096 - offset0)) return 0;

    uint64_t consumed = 4096 - offset0;
    for (int i = 1; i < 5 && consumed < len; i++) {
        uint8_t *virt_next = (uint8_t *)buf + consumed;
        uint64_t phys_next = virt_to_phys(virt_next);
        if (phys_next > 0xFFFFFFFFULL) return -1;
        qtd->buffer[i] = (uint32_t)(phys_next & ~0xFFFULL);
        consumed += 4096;
    }
    return 0;
}

// ============================================================================
// BIOS handoff
// ============================================================================
static void take_ehci_ownership(pci_device_t *dev, uint32_t hccparams) {
    uint32_t eecp = (hccparams & EHCI_HCC_EECP_MASK) >> EHCI_HCC_EECP_SHIFT;
    if (eecp < 0x40) {
        printf("EHCI: No BIOS handoff capability (EECP=%d).\n", eecp);
        return;
    }

    uint32_t legsup = read_pci(dev->bus, dev->dev, dev->func, eecp);
    if ((legsup & 0xFF) != 0x01) {
        printf("EHCI: Invalid handoff signature (0x%02x).\n", legsup & 0xFF);
        return;
    }

    // Request OS ownership
    write_pci(dev->bus, dev->dev, dev->func, eecp, legsup | (1 << 24));
    printf("EHCI: Requesting OS ownership...\n");

    // Wait for BIOS to release ownership (clear BIOS Ownership Semaphore)
    for (int i = 0; i < 5000; i++) {
        legsup = read_pci(dev->bus, dev->dev, dev->func, eecp);
        if (!(legsup & (1 << 16))) {
            printf("EHCI: BIOS released ownership after %d iterations.\n", i);
            
            // Disable SMIs in USBLEGCTLSTS (offset + 4)
            uint32_t legctlsts = read_pci(dev->bus, dev->dev, dev->func, eecp + 4);
            legctlsts &= ~0x3F; // clear SMI enables (bits 5:0)
            write_pci(dev->bus, dev->dev, dev->func, eecp + 4, legctlsts);
            
            return;
        }
        sleep_us(100);
    }

    printf("EHCI: BIOS handoff timeout, forcing ownership and disabling SMIs.\n");
    
    // Force disable SMIs if BIOS timed out
    uint32_t legctlsts = read_pci(dev->bus, dev->dev, dev->func, eecp + 4);
    legctlsts &= ~0x3F;
    write_pci(dev->bus, dev->dev, dev->func, eecp + 4, legctlsts);
}

// ============================================================================
// Transfer stubs
// ============================================================================
static int ehci_control_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                                  usb_setup_packet_t *setup, void *data, uint16_t length) {
    ehci_controller_t *ctrl = (ehci_controller_t *)hcd->hcd_data;
    uint8_t addr = dev->address;
    uint8_t qh_speed = (dev->speed == USB_SPEED_LOW) ? 1 : ((dev->speed == USB_SPEED_HIGH) ? 2 : 0);
    uint8_t data_dir_in = ((setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0);

    // For control EP0, high-speed is always 64. For FS/LS use the probed bMaxPacketSize0.
    uint32_t max_pkt = (dev->speed == USB_SPEED_HIGH) ? 64 : dev->max_packet_size;
    if (max_pkt != 8 && max_pkt != 16 && max_pkt != 32 && max_pkt != 64) {
        max_pkt = (dev->speed == USB_SPEED_HIGH) ? 64 : 8;
    }

    // Clear QH overlay before reuse to prevent stale Active bits from blocking transfer
    ctrl->async_qh->current_qtd = 0;
    ctrl->async_qh->token = 0;
    ctrl->async_qh->alt_next_qtd = EHCI_PTR_TERMINATE;

    // This QH is ASYNCLISTADDR — keep H (bit15) or the async schedule does not run.
    uint32_t qh_char = (uint32_t)(addr & 0x7F)
                     | ((uint32_t)qh_speed << 12)
                     | ((uint32_t)max_pkt << 16)
                     | (1u << 15)
                     | (1u << 27);
    ctrl->async_qh->characteristics = qh_char;

    #define TOK_SETUP() (EHCI_QTD_ACTIVE | EHCI_QTD_PID_SETUP | (3u << 10) | (8u << 16))
    #define TOK_DATA(dir_in, len, dt1) \
        (EHCI_QTD_ACTIVE | ((dir_in) ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) | (3u << 10) \
         | (((uint32_t)(len) & 0x7FFFu) << 16) | ((dt1) ? (1u << 31) : 0u))
    #define TOK_STATUS(dir_in, dt1) \
        (EHCI_QTD_ACTIVE | ((dir_in) ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) | (3u << 10) \
         | ((dt1) ? (1u << 31) : 0u))

    ehci_qtd_t *td_setup = alloc_ehci_qtd();
    td_setup->token = TOK_SETUP();
    if (ehci_fill_qtd_buffers(td_setup, setup, 8) < 0) return -1;
    td_setup->next_qtd = 0x01;
    td_setup->alt_next_qtd = 0x01;

    ehci_qtd_t *td_data = NULL;
    if (length > 0) {
        td_data = alloc_ehci_qtd();
        td_data->token = TOK_DATA(data_dir_in, length, 1);
        if (ehci_fill_qtd_buffers(td_data, data, length) < 0) return -1;
        td_data->next_qtd = 0x01;
        td_data->alt_next_qtd = 0x01;
        td_setup->next_qtd = (uint32_t)virt_to_phys(td_data);
    }

    ehci_qtd_t *td_status = alloc_ehci_qtd();
    uint8_t status_dir_in = (length > 0) ? (uint8_t)!data_dir_in : 1;
    // USB control status stage always uses DATA1.
    td_status->token = TOK_STATUS(status_dir_in, 1);
    if (td_data) td_data->next_qtd = (uint32_t)virt_to_phys(td_status);
    else td_setup->next_qtd = (uint32_t)virt_to_phys(td_status);
    td_status->next_qtd = 0x01;
    td_status->alt_next_qtd = 0x01;

    ctrl->async_qh->next_qtd = (uint32_t)virt_to_phys(td_setup);
    int ret = wait_ehci_qtd(td_status, 200000, 32);
    ctrl->async_qh->next_qtd = 0x01;
    return ret;
}

// Interrupt endpoints are serviced from the periodic frame list.
static void ehci_ensure_periodic_interrupt_qh(ehci_controller_t *ctrl) {
    if (ctrl->intr_periodic_linked) return;

    ctrl->intr_qh = (ehci_qh_t *)malloc(sizeof(ehci_qh_t) + 64);
    ctrl->intr_qh = (ehci_qh_t *)(((uint64_t)ctrl->intr_qh + 31) & ~31ULL);
    ctrl->intr_qtd = (ehci_qtd_t *)malloc(sizeof(ehci_qtd_t) + 32);
    ctrl->intr_qtd = (ehci_qtd_t *)(((uint64_t)ctrl->intr_qtd + 31) & ~31ULL);

    memset(ctrl->intr_qh, 0, sizeof(ehci_qh_t));
    ctrl->intr_qh->next_qh = EHCI_PTR_TERMINATE;
    /* capabilities: mult=1, no split transactions (HS device on root hub) */
    ctrl->intr_qh->capabilities = 0x01u;
    ctrl->intr_qh->next_qtd = EHCI_PTR_TERMINATE;
    ctrl->intr_qh->alt_next_qtd = EHCI_PTR_TERMINATE;

    uint32_t link = (uint32_t)virt_to_phys(ctrl->intr_qh) | EHCI_PTR_TYPE_QH;
    for (int i = 0; i < 1024; i++) {
        ctrl->periodic_list[i] = link;
    }
    ctrl->intr_periodic_linked = 1;
}

// ============================================================================
// Transfer stubs (unused for keyboard - we use non-blocking polling instead)
// ============================================================================
static int ehci_interrupt_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                                    uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)endpoint; (void)data; (void)length;
    return -1; // Not used: EHCI interrupt transfers handled via ehci_poll_keyboards()
}

static int ehci_bulk_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                               uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)endpoint; (void)data; (void)length;
    return -1;
}

// ============================================================================
// Hot-plug helper: reset and probe a single port
// ============================================================================
static void handle_ehci_port(ehci_controller_t *ctrl, volatile uint8_t *op, int port_idx) {
    uint32_t port_reg = EHCI_OP_PORTSC_BASE + (port_idx * 4);
    uint32_t portsc = ehci_op_read32(op, port_reg);

    if (!(portsc & EHCI_PORT_CCS)) {
        printf("EHCI: Port %d: No connect status.\n", port_idx);
        return;
    }

    printf("EHCI: Port %d: Device connected.\n", port_idx);

    // USB 2.0 spec §9.1.2: wait 100ms debounce after connect detect
    sleep(100);

    // Re-read after debounce — device may have bounced off
    portsc = ehci_op_read32(op, port_reg);
    if (!(portsc & EHCI_PORT_CCS)) {
        printf("EHCI: Port %d: Device disconnected during debounce.\n", port_idx);
        return;
    }

    // Check line status to determine device speed before reset.
    // EHCI spec §2.3.9 PORTSC bits [11:10] = [D+, D-]:
    //   00 = SE0, 01 = K-state (low-speed), 10 = J-state (full-speed), 11 = undefined
    // Per EHCI spec §4.2.2: K-state before reset means low-speed device — hand off
    // immediately without resetting (EHCI must not reset low-speed devices).
    uint32_t line_status = (portsc >> 10) & 0x03;

    if (line_status == 0x01) {
        printf("EHCI: Port %d: Low-speed device (K-state), PORTSC=0x%08X, handing off to companion.\n", port_idx, portsc);
        uint32_t new_portsc = ehci_portsc_preserve(portsc) | EHCI_PORT_OWNER;
        ehci_op_write32(op, port_reg, new_portsc);
        printf("EHCI: Port %d: Wrote PORTSC=0x%08X (PORT_OWNER set).\n", port_idx, new_portsc);
        sleep(2);
        uint32_t verify_portsc = ehci_op_read32(op, port_reg);
        printf("EHCI: Port %d: Read back PORTSC=0x%08X after handoff.\n", port_idx, verify_portsc);
        notify_ehci_companion(ctrl, port_idx);
        return;
    }

    // If already owned by companion, skip
    if (portsc & EHCI_PORT_OWNER) {
        printf("EHCI: Port %d: Owned by companion controller.\n", port_idx);
        return;
    }

    // Perform port reset — preserve PP (power), mask out W1C and PED
    printf("EHCI: Port %d: Resetting port...\n", port_idx);
    uint32_t base = ehci_portsc_preserve(portsc) & ~EHCI_PORT_PED;
    base |= EHCI_PORT_PP;  // Ensure port power stays on
    ehci_op_write32(op, port_reg, base | EHCI_PORT_RESET);
    sleep(50);  // USB spec requires 50ms reset
    ehci_op_write32(op, port_reg, base & ~EHCI_PORT_RESET);
    sleep(100); // Wait for reset completion

    // Read back port status
    portsc = ehci_op_read32(op, port_reg);
    printf("EHCI: Port %d: Post-reset PORTSC=0x%08X\n", port_idx, portsc);

    // Check if port is enabled
    if (portsc & EHCI_PORT_PED) {
        // EHCI root ports ONLY enable high-speed devices — PED=1 means high-speed
        printf("EHCI: Port %d: Device enabled (high-speed).\n", port_idx);
        register_usb_hcd(&ctrl->hcd);
        init_usb_keyboard(&ctrl->hcd, USB_SPEED_HIGH, port_idx);
    } else if (portsc & EHCI_PORT_CCS) {
        // Device connected but not enabled - hand off to companion
        printf("EHCI: Port %d: Handing off to companion (not HS).\n", port_idx);
        ehci_op_write32(op, port_reg, ehci_portsc_preserve(portsc) | EHCI_PORT_OWNER);
        notify_ehci_companion(ctrl, port_idx);
    } else {
        printf("EHCI: Port %d: Failed to enable device.\n", port_idx);
    }
}

// ============================================================================
// Hot-plug: poll ports for connect/disconnect changes
// ============================================================================

/* Forward declarations -- definitions below */
static void arm_keyboard_ehci(ehci_controller_t *ctrl);
static void register_irq_ehci(void);

void poll_ehci_ports(void) {
    for (int c = 0; c < ehci_controller_count; c++) {
        ehci_controller_t *ctrl = &ehci_controllers[c];
        if (!ctrl->initialized) continue;

        // Hot-plug: check port changes
        volatile uint8_t *op = ctrl->op_regs;
        uint32_t usbsts = ehci_op_read32(op, EHCI_OP_USBSTS);
        if (usbsts & EHCI_STS_PCD) {
            ehci_op_write32(op, EHCI_OP_USBSTS, EHCI_STS_PCD);
            for (int i = 0; i < ctrl->num_ports; i++) {
                uint32_t port_reg = EHCI_OP_PORTSC_BASE + (i * 4);
                uint32_t portsc = ehci_op_read32(op, port_reg);
                if (portsc & EHCI_PORT_CSC) {
                    // Acknowledge CSC by writing ONLY the W1C bit
                    ehci_op_write32(op, port_reg, ehci_portsc_preserve(portsc) | EHCI_PORT_CSC);

                    // Completely ignore companion-owned ports — companion controller
                    // generates CSC events during its own port resets, which is normal.
                    if (portsc & EHCI_PORT_OWNER) continue;

                    if (portsc & EHCI_PORT_CCS) {
                        // Device connected on EHCI-owned port
                        handle_ehci_port(ctrl, op, i);
                    } else {
                        // Device genuinely disconnected from EHCI-owned port
                        printf("EHCI: Port %d: Device disconnected.\n", i);
                        for (int k = 0; k < kbd_total; k++) {
                            if (kbd_list[k].hcd == &ctrl->hcd && 
                                kbd_list[k].dev && kbd_list[k].dev->port_id == i) {
                                if (k == ctrl->pending_kbd_idx) {
                                    ctrl->pending_dev = NULL;
                                    ctrl->pending_kbd_idx = -1;
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
                    }
                }
            }
        }

        // Keyboard polling: handle attached EHCI keyboards (round-robin)
        if (!ctrl->intr_qh || !ctrl->intr_qtd) continue;

        // Check if current pending transfer completed
        if (ctrl->pending_dev != NULL && ctrl->pending_kbd_idx >= 0 && ctrl->pending_kbd_idx < kbd_total) {
            if (kbd_list[ctrl->pending_kbd_idx].dev != ctrl->pending_dev) {
                ctrl->pending_dev = NULL;
                ctrl->pending_kbd_idx = -1;
            } else {
                ehci_qtd_t *qtd = ctrl->intr_qtd;
                uint32_t token = *(volatile uint32_t*)&qtd->token;
                if (!(token & EHCI_QTD_ACTIVE)) {
                    if (!(token & 0x7C)) {
                        ctrl->pending_dev->interrupt_toggle ^= 1;
                        ctrl->intr_qh->next_qtd = EHCI_PTR_TERMINATE;
                        int ki = ctrl->pending_kbd_idx;
                        uint8_t *buf_to_process = kbd_list[ki].report_buf_next;
                        if (buf_to_process) {
                            usb_keyboard_process_report(buf_to_process, ki);
                            uint8_t *temp = kbd_list[ki].report_buf;
                            kbd_list[ki].report_buf = kbd_list[ki].report_buf_next;
                            kbd_list[ki].report_buf_next = temp;
                        }
                    }
                    ctrl->pending_dev = NULL;
                    ctrl->pending_kbd_idx = -1;
                }
            }
        }

        /* Arm next keyboard via shared helper (IRQ handler also uses this) */
        arm_keyboard_ehci(ctrl);
    }
}

// ============================================================================
// Initialization
// ============================================================================
void init_ehci(pci_device_t *dev) {
    if (ehci_controller_count >= MAX_EHCI_CONTROLLERS) {
        return;
    }

    // Prevent initializing the same BAR twice (handles multi-function devices)
    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint64_t mmio_phys = bar0 & ~0x0FULL;
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);
        mmio_phys |= ((uint64_t)bar1 << 32);
    }

    for (int i = 0; i < ehci_controller_count; i++) {
        if (ehci_initialized_bars[i] == mmio_phys) {
            return;
        }
    }

    if (mmio_phys == 0) {
        printf("EHCI: No MMIO base found.\n");
        return;
    }

    ehci_controller_t *ctrl = &ehci_controllers[ehci_controller_count];
    memset(ctrl, 0, sizeof(ehci_controller_t));

    set_pci_d0(dev);

    // ---- ACPI _CRS validation of MMIO base ----
    acpi_resource_list_t res_list = {0};
    if (get_acpi_usb_resources(dev->bus, dev->dev, dev->func, &res_list)) {
        ctrl->acpi_validated = 1;
        printf("EHCI: ACPI _CRS confirmed %d resources.\n", res_list.count);
        for (int r = 0; r < res_list.count; r++) {
            acpi_resource_t *res = &res_list.resources[r];
            if (!res->valid) continue;
            if (res->type == ACPI_RES_MMIO) {
                printf("EHCI:   MMIO range 0x%08lX-0x%08lX\n",
                       (unsigned long)res->base, (unsigned long)(res->base + res->length - 1));
                uint64_t acpi_lo = res->base;
                uint64_t acpi_hi = res->base + res->length - 1;
                if (mmio_phys >= acpi_lo && mmio_phys <= acpi_hi) {
                    printf("EHCI:   PCI BAR MMIO 0x%08lX is within ACPI range -- VALID.\n",
                           (unsigned long)mmio_phys);
                } else {
                    printf("EHCI:   WARNING: PCI BAR MMIO 0x%08lX is OUTSIDE ACPI range!\n",
                           (unsigned long)mmio_phys);
                }
            }
            else if (res->type == ACPI_RES_IRQ) {
                printf("EHCI:   IRQ %u\n", res->irq);
            }
        }
    } else {
        printf("EHCI: No ACPI _CRS match -- using PCI BAR only.\n");
    }
    ctrl->mmio_phys_addr = mmio_phys;

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd);

    volatile uint8_t *cap_regs = (volatile uint8_t *)vmap_mmio(mmio_phys, 1);
    ctrl->cap_regs = cap_regs;

    uint8_t cap_length = ehci_cap_read8(cap_regs, EHCI_CAP_CAPLENGTH);
    ctrl->op_regs = cap_regs + cap_length;

    uint32_t hcsparams = ehci_cap_read32(cap_regs, EHCI_CAP_HCSPARAMS);
    uint32_t hccparams = ehci_cap_read32(cap_regs, EHCI_CAP_HCCPARAMS);
    uint16_t version = *(volatile uint16_t*)(cap_regs + EHCI_CAP_HCIVERSION);

    ctrl->num_ports = hcsparams & EHCI_HCS_N_PORTS_MASK;

    // N_PCC: how many EHCI ports each companion controller owns.
    // Used by notify_ehci_companion to route handoffs to the right UHCI.
    // If N_PCC == 0 the HCSPARAMS field isn't populated — default to 2 (ICH safe default).
    uint32_t n_pcc = (hcsparams & EHCI_HCS_N_PCC_MASK) >> EHCI_HCS_N_PCC_SHIFT;
    ctrl->n_pcc = (n_pcc > 0) ? (int)n_pcc : 2;

    take_ehci_ownership(dev, hccparams);

    volatile uint8_t *op = ctrl->op_regs;

    // Halt
    uint32_t usbcmd = ehci_op_read32(op, EHCI_OP_USBCMD);
    usbcmd &= ~EHCI_CMD_RS;
    ehci_op_write32(op, EHCI_OP_USBCMD, usbcmd);

    for (int i = 0; i < 1000; i++) {
        if (ehci_op_read32(op, EHCI_OP_USBSTS) & EHCI_STS_HALTED) break;
        sleep(10);
    }

    // Reset
    ehci_op_write32(op, EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    for (int i = 0; i < 1000; i++) {
        if (!(ehci_op_read32(op, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) break;
        sleep(10);
    }

    ehci_op_write32(op, EHCI_OP_CTRLDSSEGMENT, 0);

    // Periodic frame list
    ctrl->periodic_list = (uint32_t*)malloc(1024 * sizeof(uint32_t) + 4096);
    ctrl->periodic_list = (uint32_t*)(((uint64_t)ctrl->periodic_list + 4095) & ~4095ULL);
    ctrl->periodic_list_phys = virt_to_phys(ctrl->periodic_list);

    for (int i = 0; i < 1024; i++) {
        ctrl->periodic_list[i] = EHCI_PTR_TERMINATE;
    }

    ehci_op_write32(op, EHCI_OP_PERIODICLISTBASE, (uint32_t)ctrl->periodic_list_phys);

    ctrl->intr_qh = NULL;
    ctrl->intr_qtd = NULL;
    ctrl->intr_periodic_linked = 0;
    ctrl->pending_dev = NULL;
    ehci_ensure_periodic_interrupt_qh(ctrl);

    ctrl->async_qh = (ehci_qh_t*)malloc(sizeof(ehci_qh_t) + 64);
    ctrl->async_qh = (ehci_qh_t*)(((uint64_t)ctrl->async_qh + 31) & ~31ULL);
    memset(ctrl->async_qh, 0, sizeof(ehci_qh_t));

    uint64_t qh_phys = virt_to_phys(ctrl->async_qh);
    ctrl->async_qh->next_qh = (uint32_t)qh_phys | EHCI_PTR_TYPE_QH;
    ctrl->async_qh->characteristics = (1 << 15);
    ctrl->async_qh->next_qtd = EHCI_PTR_TERMINATE;
    ctrl->async_qh->alt_next_qtd = EHCI_PTR_TERMINATE;

    ehci_op_write32(op, EHCI_OP_ASYNCLISTADDR, (uint32_t)qh_phys);

    ehci_op_write32(op, EHCI_OP_USBSTS, 0x3F);
    ehci_op_write32(op, EHCI_OP_USBINTR, 0x00);

    ehci_op_write32(op, EHCI_OP_CONFIGFLAG, EHCI_CF_FLAG);
    sleep(100);  // USB spec: wait 100ms for port routing to settle after CF

    usbcmd = ehci_op_read32(op, EHCI_OP_USBCMD);
    usbcmd |= EHCI_CMD_RS | EHCI_CMD_ASE | EHCI_CMD_PSE;
    usbcmd = (usbcmd & ~EHCI_CMD_ITC_MASK) | (8 << 16);
    ehci_op_write32(op, EHCI_OP_USBCMD, usbcmd);

    // Enable USB interrupt on completion (for interrupt-driven keyboard)
    ehci_op_write32(op, EHCI_OP_USBINTR, EHCI_STS_USBINT | EHCI_STS_USBERRINT);

    printf("EHCI: HCSPARAMS=0x%08X  HCCPARAMS=0x%08X  num_ports=%d\n",
           hcsparams, hccparams, ctrl->num_ports);

    // Port Power Control: HCSPARAMS bit 4 (PPC) indicates whether ports
    // have individual power switching. After HCRESET, PP bits are cleared
    // and ports have NO power — all status bits read 0x00000000.
    // We must explicitly set PP on each port and wait for power to stabilize.
    int ppc = (hcsparams >> 4) & 1;
    if (ppc) {
        printf("EHCI: PPC=1, powering on %d ports...\n", ctrl->num_ports);
        for (int i = 0; i < ctrl->num_ports; i++) {
            uint32_t port_reg = EHCI_OP_PORTSC_BASE + (i * 4);
            uint32_t portsc = ehci_op_read32(op, port_reg);
            if (!(portsc & EHCI_PORT_PP)) {
                ehci_op_write32(op, port_reg, portsc | EHCI_PORT_PP);
            }
        }
        // Wait for port power to stabilize — 20ms minimum per EHCI spec
        sleep(20);
    } else {
        printf("EHCI: PPC=0, ports always powered.\n");
        sleep(20); // Still give some time for devices to signal
    }

    ctrl->hcd.name = "EHCI";
    ctrl->hcd.control_transfer = ehci_control_transfer;
    ctrl->hcd.interrupt_transfer = ehci_interrupt_transfer;
    ctrl->hcd.bulk_transfer = ehci_bulk_transfer;
    ctrl->hcd.hcd_data = ctrl;
    ctrl->pending_dev = NULL;
    ctrl->pending_kbd_idx = -1;
    ctrl->initialized = 1;

    ehci_initialized_bars[ehci_controller_count] = mmio_phys;
    ehci_controller_count++;

    printf("EHCI: Initialized EHCI.\n");

    for (int i = 0; i < ctrl->num_ports; i++) {
        uint32_t port_reg = EHCI_OP_PORTSC_BASE + (i * 4);
        uint32_t portsc = ehci_read_portsc_with_settle(op, port_reg, ppc, 12, 5);

        printf("EHCI: Port %d: PORTSC=0x%08X (PP=%d CCS=%d PED=%d)\n",
               i, portsc, !!(portsc & EHCI_PORT_PP),
               !!(portsc & EHCI_PORT_CCS), !!(portsc & EHCI_PORT_PED));
        // If still zero after settle retries, skip for now.
        // If still 0x00000000 after that single retry, the port does not exist.
        if (portsc == 0x00000000) {
            printf("EHCI: Port %d: Still reads 0x00000000 after settle retries, skipping.\n", i);
            continue;
        }

        if (portsc & EHCI_PORT_CCS) {
            handle_ehci_port(ctrl, op, i);
        }

        // Clear any CSC generated during port handling (reset/handoff)
        portsc = ehci_op_read32(op, port_reg);
        if (portsc & EHCI_PORT_CSC) {
            ehci_op_write32(op, port_reg, ehci_portsc_preserve(portsc) | EHCI_PORT_CSC);
        }
    }

    // Clear global PCD to prevent poll_ports_ehci from re-processing init events
    ehci_op_write32(op, EHCI_OP_USBSTS, EHCI_STS_PCD);

    // Register interrupt handler for IRQ-driven keyboard processing
    register_irq_ehci();
}

// ============================================================================
// Interrupt-driven keyboard handler (called from PCI IRQ)
// ============================================================================

/* Extracted keyboard arming helper — used by both IRQ handler and poll fallback */
static void arm_keyboard_ehci(ehci_controller_t *ctrl) {
    if (!ctrl->intr_qh || !ctrl->intr_qtd) return;
    if (kbd_total == 0) return;

    /* If a transfer is already pending, don't overwrite it */
    if (ctrl->pending_dev != NULL) return;

    int start_idx = (ctrl->pending_kbd_idx + 1) % kbd_total;
    for (int attempts = 0; attempts < kbd_total; attempts++) {
        int k = (start_idx + attempts) % kbd_total;
        if (kbd_list[k].hcd != &ctrl->hcd) continue;

        usb_device_t *dev = kbd_list[k].dev;
        uint8_t *buf = kbd_list[k].report_buf_next;
        if (!dev || !buf) continue;

        uint8_t qh_speed = (dev->speed == USB_SPEED_LOW) ? 1 : ((dev->speed == USB_SPEED_HIGH) ? 2 : 0);
        uint32_t qh_char = (uint32_t)(dev->address & 0x7F)
                         | (((uint32_t)1 & 0xFu) << 8)
                         | ((uint32_t)qh_speed << 12)
                         | ((uint32_t)8 << 16)
                         | (1u << 14);
        ctrl->intr_qh->characteristics = qh_char;
        ctrl->intr_qh->current_qtd = 0;
        ctrl->intr_qh->next_qtd = EHCI_PTR_TERMINATE;
        ctrl->intr_qh->alt_next_qtd = EHCI_PTR_TERMINATE;
        ctrl->intr_qh->token = 0;

        ehci_qtd_t *qtd = ctrl->intr_qtd;
        memset(qtd, 0, sizeof(ehci_qtd_t));
        qtd->next_qtd = EHCI_PTR_TERMINATE;
        qtd->alt_next_qtd = EHCI_PTR_TERMINATE;
        uint32_t token = EHCI_QTD_ACTIVE | EHCI_QTD_PID_IN | (3u << 10)
                       | (((uint32_t)8 & 0x7FFFu) << 16)
                       | EHCI_QTD_IOC
                       | (((uint32_t)(dev->interrupt_toggle & 1)) << 31);
        qtd->token = token;
        if (ehci_fill_qtd_buffers(qtd, buf, 8) < 0) {
            continue;
        }
        ctrl->intr_qh->next_qtd = (uint32_t)virt_to_phys(qtd);
        ctrl->pending_dev = dev;
        ctrl->pending_kbd_idx = k;
        break;
    }
}

static void irq_handler_ehci(void) {
    for (int c = 0; c < ehci_controller_count; c++) {
        ehci_controller_t *ctrl = &ehci_controllers[c];
        if (!ctrl->initialized || !ctrl->intr_qh || !ctrl->intr_qtd) continue;

        volatile uint8_t *op = ctrl->op_regs;
        uint32_t usbsts = ehci_op_read32(op, EHCI_OP_USBSTS);
        if (!(usbsts & (EHCI_STS_USBINT | EHCI_STS_USBERRINT))) continue;

        ehci_op_write32(op, EHCI_OP_USBSTS, EHCI_STS_USBINT | EHCI_STS_USBERRINT);

        /* Check if the periodic transfer completed */
        if (ctrl->pending_dev == NULL ||
            ctrl->pending_kbd_idx < 0 ||
            ctrl->pending_kbd_idx >= kbd_total) {
            /* No valid pending transfer -- reset and re-arm */
            ctrl->pending_dev = NULL;
            ctrl->pending_kbd_idx = -1;
            ctrl->intr_qh->next_qtd = EHCI_PTR_TERMINATE;
            arm_keyboard_ehci(ctrl);
            continue;
        }

        kbd_entry_t *entry = &kbd_list[ctrl->pending_kbd_idx];
        if (entry->dev != ctrl->pending_dev || !entry->report_buf_next) {
            ctrl->pending_dev = NULL;
            ctrl->pending_kbd_idx = -1;
            ctrl->intr_qh->next_qtd = EHCI_PTR_TERMINATE;
            arm_keyboard_ehci(ctrl);
            continue;
        }

        ehci_qtd_t *qtd = ctrl->intr_qtd;
        uint32_t token = *(volatile uint32_t*)&qtd->token;
        if (token & EHCI_QTD_ACTIVE) {
            /* Transfer still in progress -- shouldn't happen on interrupt, but guard */
            continue;
        }

        /* Transfer completed */
        if (!(token & 0x7C)) {
            /* Success -- toggle data PID and process report */
            ctrl->pending_dev->interrupt_toggle ^= 1;
            ctrl->intr_qh->next_qtd = EHCI_PTR_TERMINATE;
            int ki = ctrl->pending_kbd_idx;
            uint8_t *bp = entry->report_buf_next;
            if (bp) {
                usb_keyboard_process_report(bp, ki);
                uint8_t *temp = entry->report_buf;
                entry->report_buf = entry->report_buf_next;
                entry->report_buf_next = temp;
            }
        } else {
            /* Error on transfer -- log and back off to avoid busy loop */
            ctrl->pending_dev = NULL;
            ctrl->pending_kbd_idx = -1;
            continue;
        }
        /* Success -- clear pending and re-arm */
        ctrl->pending_dev = NULL;
        ctrl->pending_kbd_idx = -1;
        arm_keyboard_ehci(ctrl);
    }
}

static void register_irq_ehci(void) {
    static int done = 0;
    if (done) return;
    register_pci_interrupt_handler(irq_handler_ehci);
    done = 1;
}
