#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>
#include <io/ohci.h>
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

static ohci_controller_t ohci_ctrl = {0};
static uint64_t ohci_initialized_bar = 0;  // Track initialized BAR to prevent duplicates

// ============================================================================
// MMIO helpers
// ============================================================================
static uint32_t read_ohci(volatile uint32_t *base, uint32_t reg) {
    return *(volatile uint32_t*)((uint8_t*)base + reg);
}

static void write_ohci(volatile uint32_t *base, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)((uint8_t*)base + reg) = val;
}

static uint32_t read_ohci_port_with_settle(volatile uint32_t *base, uint32_t port_reg,
                                           int force_port_power, int attempts, uint64_t delay_ms) {
    uint32_t status = 0;
    for (int i = 0; i < attempts; i++) {
        status = read_ohci(base, port_reg);
        if (status != 0x00000000) return status;
        if (force_port_power) {
            // In per-port switching mode (PSM=1), set PPS on each port.
            write_ohci(base, port_reg, OHCI_PORT_PPS);
        }
        if (delay_ms) sleep(delay_ms);
    }
    return status;
}

static ohci_ed_t *alloc_ohci_ed(void) {
    ohci_ed_t *ed = (ohci_ed_t *)malloc(sizeof(ohci_ed_t) + 16);
    ed = (ohci_ed_t *)(((uint64_t)ed + 15) & ~15ULL);
    memset(ed, 0, sizeof(ohci_ed_t));
    return ed;
}

static ohci_td_t *alloc_ohci_td(void) {
    ohci_td_t *td = (ohci_td_t *)malloc(sizeof(ohci_td_t) + 16);
    td = (ohci_td_t *)(((uint64_t)td + 15) & ~15ULL);
    memset(td, 0, sizeof(ohci_td_t));
    return td;
}

// Check TD completion without blocking
static int check_ohci_td(ohci_td_t *td) {
    uint32_t ctrl = *(volatile uint32_t*)&td->control;
    uint8_t cc = (ctrl >> 28) & 0x0F;
    if (cc == 0x0F) {
        return -1; // not yet accessed
    }
    if (cc != 0) {
        return -2; // error
    }
    return 0; // success
}

// Blocking wait for TD completion (used by control transfers)
static int wait_ohci_td(ohci_td_t *td) {
    for (int i = 0; i < 100000; i++) {
        int ret = check_ohci_td(td);
        if (ret != -1) return ret;
        sleep_us(100);
    }
    return -2; // timeout
}

// ============================================================================
// Transfer stubs
// ============================================================================
static int ohci_control_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                                  usb_setup_packet_t *setup, void *data, uint16_t length) {
    (void)hcd;
    uint32_t addr = dev->address;
    int ls = (dev->speed == USB_SPEED_LOW);
    uint8_t data_dir_in = ((setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0);
    uint32_t max_pkt = dev->max_packet_size;
    if (max_pkt != 8 && max_pkt != 16 && max_pkt != 32 && max_pkt != 64) {
        max_pkt = 8;
    }

    ohci_ed_t *ed = alloc_ohci_ed();
    ed->control = addr | (0 << 7) | (ls << 13) | (max_pkt << 16); // EP0

    // Setup TD
    ohci_td_t *td_setup = alloc_ohci_td();
    td_setup->control = (0 << 19) | (2 << 24) | (0x0F << 28); // R=0, DP=SETUP(00), T=DATA0(10), CC=0xF
    td_setup->cbp = (uint32_t)virt_to_phys(setup);
    td_setup->be = td_setup->cbp + 7;
    td_setup->next_td = 0;  // Will be overwritten if there's a data phase

    // Data TD (optional)
    ohci_td_t *td_data = NULL;
    if (length > 0) {
        uint32_t data_dp = data_dir_in ? 2u : 1u;
        uint32_t data_round = data_dir_in ? (1u << 18) : 0u;
        td_data = alloc_ohci_td();
        td_data->control = data_round | (data_dp << 19) | (3u << 24) | (0x0Fu << 28); // DATA1
        td_data->cbp = (uint32_t)virt_to_phys(data);
        td_data->be = td_data->cbp + length - 1;
        td_setup->next_td = (uint32_t)virt_to_phys(td_data);
    }

    // Status TD is always DATA1 with opposite direction to the data stage.
    ohci_td_t *td_status = alloc_ohci_td();
    uint32_t status_dp = (length > 0) ? (data_dir_in ? 1u : 2u) : 2u;
    td_status->control = (status_dp << 19) | (3u << 24) | (0x0Fu << 28);

    // OHCI ED queue ends when HeadP == TailP, so use an explicit dummy tail TD.
    ohci_td_t *td_tail = alloc_ohci_td();
    td_tail->next_td = 0;
    td_status->next_td = (uint32_t)virt_to_phys(td_tail);
    if (td_data) td_data->next_td = (uint32_t)virt_to_phys(td_status);
    else td_setup->next_td = (uint32_t)virt_to_phys(td_status);

    ed->head_td = (uint32_t)virt_to_phys(td_setup);
    ed->tail_td = (uint32_t)virt_to_phys(td_tail);

    write_ohci(ohci_ctrl.regs, OHCI_CTRL_HEAD_ED, (uint32_t)virt_to_phys(ed));
    write_ohci(ohci_ctrl.regs, OHCI_CMDSTATUS, OHCI_CMDSTS_CLF);
    int ret = wait_ohci_td(td_status);
    write_ohci(ohci_ctrl.regs, OHCI_CTRL_HEAD_ED, 0);

    return ret;
}

static int ohci_interrupt_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                                    uint8_t endpoint, void *data, uint16_t length) {
    (void)endpoint;

    // If there's already a pending transfer, return busy
    if (ohci_ctrl.pending_dev != NULL) {
        return -1;
    }

    uint32_t addr = dev->address;
    int ls = (dev->speed == USB_SPEED_LOW);

    // Use pre-allocated ED + 2-TD chain (data + tail)
    ohci_ed_t *ed = ohci_ctrl.pending_ed;
    ed->control = addr | (endpoint << 7) | (ls << 13) | (8 << 16); // D=00 (from TD), maxpkt 8

    ohci_td_t *td = ohci_ctrl.pending_td;
    ohci_td_t *tail = ohci_ctrl.pending_tail;

    td->next_td = (uint32_t)virt_to_phys(tail);
    // Toggle bits [25:24]: 00=DATA0, 11=DATA1 (NOT 10=carry, that's ED-only)
    uint32_t toggle_bits = (dev->interrupt_toggle != 0) ? 0x03000000 : 0x00000000;
    td->control = (1 << 18) | (2 << 19) | toggle_bits | (0x0F << 28);
    td->cbp = (uint32_t)virt_to_phys(data);
    td->be = td->cbp + length - 1;

    tail->next_td = 0; // Sentinel — HC stops when head == tail

    ed->head_td = (uint32_t)virt_to_phys(td);
    ed->tail_td = (uint32_t)virt_to_phys(tail);

    // Add ED to bulk list (since we don't have interrupt schedule set up)
    ed->next_ed = read_ohci(ohci_ctrl.regs, OHCI_BULK_HEAD_ED);
    write_ohci(ohci_ctrl.regs, OHCI_BULK_HEAD_ED, (uint32_t)virt_to_phys(ed));
    write_ohci(ohci_ctrl.regs, OHCI_CMDSTATUS, OHCI_CMDSTS_BLF);

    // Save pending state including previous next pointer for removal
    ohci_ctrl.pending_dev = dev;
    ohci_ctrl.pending_buf = (uint8_t*)data;
    ohci_ctrl.pending_ed = ed;
    ohci_ctrl.pending_td = td;
    ohci_ctrl.pending_ed_next = ed->next_ed; // Save old head for later

    return 0;
}

static int ohci_bulk_transfer(usb_hcd_t *hcd, usb_device_t *dev,
                               uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)endpoint; (void)data; (void)length;
    return -1;
}

// ============================================================================
// Hot-plug: poll ports for connect/disconnect changes
// ============================================================================
void poll_ohci_ports(void) {
    if (!ohci_ctrl.initialized) return;

    volatile uint32_t *regs = ohci_ctrl.regs;

    // Clear RHSC if set (acknowledge the interrupt)
    uint32_t intr_status = read_ohci(regs, OHCI_INTRSTATUS);
    if (intr_status & OHCI_INTR_RHSC) {
        write_ohci(regs, OHCI_INTRSTATUS, OHCI_INTR_RHSC);
    }

    // Always check ports for changes (don't gate behind RHSC)
    for (int i = 0; i < ohci_ctrl.num_ports; i++) {
        uint32_t port_reg = OHCI_RHPORTSTATUS_BASE + (i * 4);
        uint32_t status = read_ohci(regs, port_reg);

        if (status & OHCI_PORT_CSC) {
            write_ohci(regs, port_reg, OHCI_PORT_CSC);

            if (status & OHCI_PORT_CCS) {
                int ls = (status & OHCI_PORT_LSDA) ? 1 : 0;
                printf("OHCI: Port %d: Device connected (speed=%s).\n", i, ls ? "LOW" : "FULL");

                // Clear pending state on new connection
                ohci_ctrl.pending_dev = NULL;
                ohci_ctrl.pending_buf = NULL;
                ohci_ctrl.pending_ed_next = 0;

                write_ohci(regs, port_reg, OHCI_PORT_PRS);
                for (int j = 0; j < 1000; j++) {
                    if (read_ohci(regs, port_reg) & OHCI_PORT_PRSC) break;
                    sleep_us(100);
                }
                write_ohci(regs, port_reg, OHCI_PORT_PRSC);

                register_usb_hcd(&ohci_ctrl.hcd);
                init_usb_keyboard(&ohci_ctrl.hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
            } else {
                // Device disconnected
                printf("OHCI: Port %d: Device disconnected.\n", i);
                // Clear pending and remove keyboard from list
                for (int k = 0; k < kbd_total; k++) {
                    if (kbd_list[k].hcd == &ohci_ctrl.hcd && 
                        kbd_list[k].dev && kbd_list[k].dev->port_id == i) {
                        // Clear pending state if this keyboard was being polled
                        if (kbd_list[k].dev == ohci_ctrl.pending_dev) {
                            ohci_ctrl.pending_dev = NULL;
                            ohci_ctrl.pending_buf = NULL;
                            ohci_ctrl.pending_ed_next = 0;
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
                ohci_ctrl.pending_dev = NULL;
                ohci_ctrl.pending_buf = NULL;
                ohci_ctrl.pending_ed_next = 0;
            }
        }
    }

    // Keyboard polling: check pending completion and re-arm
    if (ohci_ctrl.pending_dev && ohci_ctrl.pending_td) {
        int ret = check_ohci_td(ohci_ctrl.pending_td);
        if (ret == 0 || ret == -2) {
            uint32_t bulk_head = read_ohci(ohci_ctrl.regs, OHCI_BULK_HEAD_ED);
            if (bulk_head == (uint32_t)virt_to_phys(ohci_ctrl.pending_ed)) {
                write_ohci(ohci_ctrl.regs, OHCI_BULK_HEAD_ED, ohci_ctrl.pending_ed_next);
            }
            if (ret == 0) {
                usb_device_t *dev = ohci_ctrl.pending_dev;
                uint8_t *buf = ohci_ctrl.pending_buf;
                
                // Validate both device and buffer before processing
                if (buf && dev) {
                    dev->interrupt_toggle ^= 1;
                    // Find keyboard and swap buffers (ping-pong)
                    int ki = kbd_find_index(dev);
                    if (ki >= 0 && ki < kbd_total) {
                        usb_keyboard_process_report(buf, ki);
                        // Swap buffers for next transfer
                        uint8_t *temp = kbd_list[ki].report_buf;
                        kbd_list[ki].report_buf = kbd_list[ki].report_buf_next;
                        kbd_list[ki].report_buf_next = temp;
                    }
                }
            }
            ohci_ctrl.pending_dev = NULL;
            ohci_ctrl.pending_buf = NULL;
            ohci_ctrl.pending_ed_next = 0;
        }
    }
    // Arm next OHCI keyboard if none pending
    if (!ohci_ctrl.pending_dev) {
        for (int k = 0; k < kbd_total; k++) {
            usb_hcd_t *hcd = kbd_list[k].hcd;
            usb_device_t *dev = kbd_list[k].dev;
            uint8_t *buf = kbd_list[k].report_buf_next;  // Use the "next" buffer for hardware writes
            if (hcd && dev && strcmp(hcd->name, "OHCI") == 0) {
                if (!buf) continue;
                int ret = ohci_ctrl.hcd.interrupt_transfer(&ohci_ctrl.hcd, dev, 1, buf, 8);
                if (ret == 0) break;
            }
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================
void init_ohci(pci_device_t *dev) {
    printf("OHCI: init_ohci() called (PCI %02X:%02X.%X).\n", dev->bus, dev->dev, dev->func);
    
    // Only one OHCI controller is supported — skip duplicates silently.
    if (ohci_ctrl.initialized) {
        printf("OHCI: Already initialized, skipping.\n");
        return;
    }

    // Prevent initializing the same BAR twice (handles multi-function devices)
    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint64_t mmio_phys = bar0 & ~0x0FULL;
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);
        mmio_phys |= ((uint64_t)bar1 << 32);
    }

    if (ohci_initialized_bar != 0 && ohci_initialized_bar == mmio_phys) {
        return;
    }
    ohci_initialized_bar = mmio_phys;

    if (mmio_phys == 0) {
        printf("OHCI: No MMIO base found.\n");
        return;
    }

    set_pci_d0(dev);

    // ---- ACPI _CRS validation of MMIO base ----
    acpi_resource_list_t res_list = {0};
    if (get_acpi_usb_resources(dev->bus, dev->dev, dev->func, &res_list)) {
        ohci_ctrl.acpi_validated = 1;
        printf("OHCI: ACPI _CRS confirmed %d resources.\n", res_list.count);
        for (int r = 0; r < res_list.count; r++) {
            acpi_resource_t *res = &res_list.resources[r];
            if (!res->valid) continue;
            if (res->type == ACPI_RES_MMIO) {
                printf("OHCI:   MMIO range 0x%08lX-0x%08lX\n",
                       (unsigned long)res->base, (unsigned long)(res->base + res->length - 1));
                uint64_t acpi_lo = res->base;
                uint64_t acpi_hi = res->base + res->length - 1;
                if (mmio_phys >= acpi_lo && mmio_phys <= acpi_hi) {
                    printf("OHCI:   PCI BAR MMIO 0x%08lX is within ACPI range -- VALID.\n",
                           (unsigned long)mmio_phys);
                } else {
                    printf("OHCI:   WARNING: PCI BAR MMIO 0x%08lX is OUTSIDE ACPI range!\n",
                           (unsigned long)mmio_phys);
                }
            }
            else if (res->type == ACPI_RES_IRQ) {
                printf("OHCI:   IRQ %u\n", res->irq);
            }
        }
    } else {
        printf("OHCI: No ACPI _CRS match -- using PCI BAR only.\n");
    }
    ohci_ctrl.mmio_phys_addr = mmio_phys;

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd);

    volatile uint32_t *regs = (volatile uint32_t *)vmap_mmio(mmio_phys, 1);
    ohci_ctrl.regs = regs;

    uint32_t rev = read_ohci(regs, OHCI_REVISION);

    // OHCI BIOS Handoff
    uint32_t ctrl = read_ohci(regs, OHCI_CONTROL);
    if (ctrl & (1 << 8)) { // OHCI_CTRL_IR (InterruptRouting)
        printf("OHCI: Requesting OS ownership...\n");
        write_ohci(regs, OHCI_CMDSTATUS, (1 << 3)); // OwnershipChangeRequest (OCR)
        for (int i = 0; i < 1000; i++) {
            ctrl = read_ohci(regs, OHCI_CONTROL);
            if (!(ctrl & (1 << 8))) {
                printf("OHCI: BIOS released ownership.\n");
                break;
            }
            sleep_us(100);
        }
    }

    write_ohci(regs, OHCI_CONTROL, OHCI_CTRL_HCFS_RESET);
    sleep(10);

    write_ohci(regs, OHCI_CMDSTATUS, OHCI_CMDSTS_HCR);
    for (int i = 0; i < 1000; i++) {
        if (!(read_ohci(regs, OHCI_CMDSTATUS) & OHCI_CMDSTS_HCR)) break;
        sleep_us(100);
    }

    ohci_ctrl.hcca = (ohci_hcca_t *)malloc(sizeof(ohci_hcca_t) + 256);
    ohci_ctrl.hcca = (ohci_hcca_t *)(((uint64_t)ohci_ctrl.hcca + 255) & ~255ULL);
    memset(ohci_ctrl.hcca, 0, sizeof(ohci_hcca_t));
    ohci_ctrl.hcca_phys = virt_to_phys(ohci_ctrl.hcca);

    write_ohci(regs, OHCI_HCCA, (uint32_t)ohci_ctrl.hcca_phys);
    write_ohci(regs, OHCI_CTRL_HEAD_ED, 0);
    write_ohci(regs, OHCI_BULK_HEAD_ED, 0);

    uint32_t fminterval = read_ohci(regs, OHCI_FMINTERVAL);
    uint32_t fit = fminterval & (1 << 31);
    write_ohci(regs, OHCI_FMINTERVAL, (fit ^ (1 << 31)) | (0x2778 << 16) | 0x2EDF);
    write_ohci(regs, OHCI_PERIODICSTART, 0x2A2F);

    write_ohci(regs, OHCI_INTRSTATUS, 0xFFFFFFFF);
    write_ohci(regs, OHCI_INTRENABLE, 0);

    write_ohci(regs, OHCI_CONTROL,
               OHCI_CTRL_HCFS_OPER | OHCI_CTRL_PLE | OHCI_CTRL_CLE | OHCI_CTRL_BLE | 0x03);

    ohci_ctrl.hcd.name = "OHCI";
    ohci_ctrl.hcd.control_transfer = ohci_control_transfer;
    ohci_ctrl.hcd.interrupt_transfer = ohci_interrupt_transfer;
    ohci_ctrl.hcd.bulk_transfer = ohci_bulk_transfer;
    ohci_ctrl.hcd.hcd_data = &ohci_ctrl;

    uint32_t rha = read_ohci(regs, OHCI_RHDESCRIPTORA);
    ohci_ctrl.num_ports = (rha & 0xFF);
    printf("OHCI: HcRhDescriptorA=0x%08X  num_ports=%d\n", rha, ohci_ctrl.num_ports);

    int nps = (rha >> 9) & 1;
    int psm = (rha >> 8) & 1;
    int per_port_power = (!nps && psm);
    uint32_t potpgt_ms = ((rha >> 24) & 0xFF) * 2;
    if (potpgt_ms == 0) potpgt_ms = 100; // Default 100ms if not specified
    if (nps) {
        printf("OHCI: NPS=1, ports always powered.\n");
        sleep(100); // Give ports time to stabilise anyway
    } else if (!psm) {
        printf("OHCI: NPS=0 PSM=0, enabling global port power (POTPGT=%dms)...\n", potpgt_ms);
        // Set LPSC (bit 16) in HcRhStatus = Set Global Power
        write_ohci(regs, OHCI_RHSTATUS, (1 << 16));
        sleep(potpgt_ms);
    } else {
        printf("OHCI: NPS=0 PSM=1, enabling per-port power on %d ports (POTPGT=%dms)...\n",
               ohci_ctrl.num_ports, potpgt_ms);
        for (int i = 0; i < ohci_ctrl.num_ports; i++) {
            uint32_t port_reg = OHCI_RHPORTSTATUS_BASE + (i * 4);
            write_ohci(regs, port_reg, OHCI_PORT_PPS);
        }
        sleep(potpgt_ms);
    }

    // PCI read flush to ensure MMIO writes are visible
    read_pci(dev->bus, dev->dev, dev->func, 0x00);

    // Debug: print port status after power-on
    for (int i = 0; i < ohci_ctrl.num_ports; i++) {
        uint32_t port_reg = OHCI_RHPORTSTATUS_BASE + (i * 4);
        uint32_t status = read_ohci_port_with_settle(regs, port_reg, per_port_power, 8, 5);
        printf("OHCI: Port %d: status=0x%08X after power-on\n", i, status);
    }

    ohci_ctrl.initialized = 1;
    ohci_ctrl.pending_dev = NULL;
    ohci_ctrl.pending_buf = NULL;
    ohci_ctrl.pending_ed = alloc_ohci_ed();
    ohci_ctrl.pending_td = alloc_ohci_td();
    ohci_ctrl.pending_tail = alloc_ohci_td();
    ohci_ctrl.pending_ed_next = 0;

    printf("OHCI: Initialized OHCI (%d ports).\n", ohci_ctrl.num_ports);

    // Initial port scan (like EHCI/xHCI) - detect already-connected devices
    for (int i = 0; i < ohci_ctrl.num_ports; i++) {
        uint32_t port_reg = OHCI_RHPORTSTATUS_BASE + (i * 4);
        uint32_t status = read_ohci_port_with_settle(regs, port_reg, per_port_power, 12, 5);

        // If a port still reads 0 after settle retries, skip for now.
        if (status == 0x00000000) {
            printf("OHCI: Port %d: Still reads 0x00000000 after settle retries, skipping.\n", i);
            continue;
        }

        if (status & OHCI_PORT_CCS) {
            int ls = (status & OHCI_PORT_LSDA) ? 1 : 0;
            printf("OHCI: Port %d: Device detected (speed=%s).\n", i, ls ? "LOW" : "FULL");

            // Reset port
            printf("OHCI: Port %d: Resetting port...\n", i);
            write_ohci(regs, port_reg, OHCI_PORT_PRS);
            for (int j = 0; j < 1000; j++) {
                if (read_ohci(regs, port_reg) & OHCI_PORT_PRSC) break;
                sleep_us(100);
            }

            write_ohci(regs, port_reg, OHCI_PORT_PRSC);
            sleep(2);

            // Clear CSC
            if (read_ohci(regs, port_reg) & OHCI_PORT_CSC) {
                write_ohci(regs, port_reg, OHCI_PORT_CSC);
            }

            write_ohci(regs, port_reg, OHCI_PORT_PES);
            sleep(10);
            
            // Verify port is still enabled after reset
            status = read_ohci(regs, port_reg);
            if (status & OHCI_PORT_PES) {
                register_usb_hcd(&ohci_ctrl.hcd);
                init_usb_keyboard(&ohci_ctrl.hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
                printf("OHCI: Port %d: Keyboard initialized.\n", i);
            } else {
                printf("OHCI: Port %d: Port not enabled after reset.\n", i);
            }
        }
    }
}

bool is_ohci_ready(void) {
    return ohci_ctrl.initialized;
}

void ohci_rescan_ports(int port_hint) {
    if (!ohci_ctrl.initialized) return;
    volatile uint32_t *regs = ohci_ctrl.regs;
    if (!regs) {
        printf("OHCI: No MMIO mapping available.\n");
        return;
    }
    uint32_t rha = read_ohci(regs, OHCI_RHDESCRIPTORA);
    int nps = (rha >> 9) & 1;
    int psm = (rha >> 8) & 1;
    int per_port_power = (!nps && psm);

    int start_port = 0;
    int end_port = ohci_ctrl.num_ports;
    if (port_hint >= 0 && port_hint < ohci_ctrl.num_ports) {
        start_port = port_hint;
        end_port = port_hint + 1;
        printf("OHCI: Rescanning handoff port %d.\n", port_hint);
    } else {
        printf("OHCI: Rescanning %d ports...\n", ohci_ctrl.num_ports);
    }

    for (int i = start_port; i < end_port; i++) {
        uint32_t port_reg = OHCI_RHPORTSTATUS_BASE + (i * 4);
        uint32_t status0 = read_ohci(regs, port_reg);
        uint32_t status = status0;
        if (status == 0x00000000) {
            status = read_ohci_port_with_settle(regs, port_reg, per_port_power, 8, 5);
        }
        printf("OHCI: Port %d: Initial status=0x%08X Settled status=0x%08X (CCS=%d, PES=%d, PRSC=%d)\n",
               i, status0, status,
               !!(status & OHCI_PORT_CCS), !!(status & OHCI_PORT_PES), !!(status & OHCI_PORT_PRSC));
        // If still zero after retries, skip for now.
        if (status == 0x00000000) {
            printf("OHCI: Port %d: Still 0x00000000 after settle retries, skipping.\n", i);
            continue;
        }

        if (status & OHCI_PORT_CSC) {
            write_ohci(regs, port_reg, OHCI_PORT_CSC);
            status = read_ohci(regs, port_reg);
        }

        if (status & OHCI_PORT_PES) {
            printf("OHCI: Port %d: Skipping (already enabled).\n", i);
            continue;
        }

        if (!(status & OHCI_PORT_CCS)) {
            // Quick check: give the routing matrix just 5ms to settle
            sleep(5);
            status = read_ohci(regs, port_reg);
            if (status & OHCI_PORT_CSC) {
                write_ohci(regs, port_reg, OHCI_PORT_CSC);
                status = read_ohci(regs, port_reg);
            }
            if (!(status & OHCI_PORT_CCS)) {
                printf("OHCI: Port %d: No device, skipping.\n", i);
                continue;
            }
        }

        sleep(100);

        int ls = (status & OHCI_PORT_LSDA) ? 1 : 0;
        printf("OHCI: Port %d: Companion handoff device (speed=%s).\n", i, ls ? "LOW" : "FULL");

        ohci_ctrl.pending_dev = NULL;
        ohci_ctrl.pending_buf = NULL;
        ohci_ctrl.pending_ed_next = 0;

        write_ohci(regs, port_reg, OHCI_PORT_PRS);
        for (int j = 0; j < 1000; j++) {
            if (read_ohci(regs, port_reg) & OHCI_PORT_PRSC) break;
            sleep_us(100);
        }

        write_ohci(regs, port_reg, OHCI_PORT_PRSC);
        sleep(2);
        write_ohci(regs, port_reg, OHCI_PORT_PES);
        sleep(10);

        status = read_ohci(regs, port_reg);
        if (status & OHCI_PORT_PES) {
            register_usb_hcd(&ohci_ctrl.hcd);
            init_usb_keyboard(&ohci_ctrl.hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
            printf("OHCI: Port %d: Handoff device enumerated.\n", i);
        } else {
            printf("OHCI: Port %d: Port not enabled after handoff reset.\n", i);
        }
    }
}
