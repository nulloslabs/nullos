#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/log.h>
#include <main/string.h>
#include <io/uhci.h>
#include <io/usb.h>
#include <io/pci.h>
#include <io/io.h>
#include <io/usb_keyboard.h>
#include <io/time.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define UHCI_MAX_CONTROL_DATA 512
#define UHCI_CONTROL_DMA_DATA_OFFSET 16
#define UHCI_CONTROL_DMA_TD_OFFSET \
    (UHCI_CONTROL_DMA_DATA_OFFSET + UHCI_MAX_CONTROL_DATA)
#define UHCI_PORT_RWC (UHCI_PORT_CSC | UHCI_PORT_PEDC)
#define UHCI_PORT_RW  (UHCI_PORT_PED | UHCI_PORT_RESET | UHCI_PORT_SUSP)

static uhci_controller_t uhci_controllers[MAX_UHCI_CONTROLLERS];
static int uhci_count = 0;

static uhci_td_t *uhci_alloc_td(void) {
    void *phys = pmalloc_dma32();
    if (!phys) return NULL;
    uhci_td_t *td = (uhci_td_t *)phys_to_virt((uint64_t)phys);
    td->link_ptr = UHCI_PTR_TERMINATE;
    return td;
}

static uhci_qh_t *uhci_alloc_qh(void) {
    void *phys = pmalloc_dma32();
    if (!phys) return NULL;
    uhci_qh_t *qh = (uhci_qh_t *)phys_to_virt((uint64_t)phys);
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
    if (status & UHCI_TD_STATUS_ERROR_MASK) {
        return -2; // error
    }
    return 0; // success
}

static int uhci_control_transfer(usb_hcd_t *hcd, usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t length) {
    if (!hcd || !dev || !setup || length > UHCI_MAX_CONTROL_DATA ||
        (length && !data)) return -1;

    uhci_controller_t *ctrl = (uhci_controller_t *)hcd->hcd_data;
    uint8_t addr = dev->address;
    int low_speed = (dev->speed == USB_SPEED_LOW);
    uint16_t max_packet = dev->max_packet_size;
    if (max_packet != 8 && max_packet != 16 &&
        max_packet != 32 && max_packet != 64) return -1;

    void *dma_page_raw = pmalloc_dma32();
    if (!dma_page_raw) return -1;
    uint64_t dma_phys = (uint64_t)dma_page_raw;
    uint8_t *dma_page = (uint8_t *)phys_to_virt(dma_phys);
    usb_setup_packet_t *dma_setup = (usb_setup_packet_t *)dma_page;
    uint8_t *dma_data = dma_page + UHCI_CONTROL_DMA_DATA_OFFSET;
    uhci_td_t *tds = (uhci_td_t *)(dma_page + UHCI_CONTROL_DMA_TD_OFFSET);
    uint64_t tds_phys = dma_phys + UHCI_CONTROL_DMA_TD_OFFSET;
    bool data_in = (setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0;
    int data_td_count = length ? (length + max_packet - 1) / max_packet : 0;
    int status_td_index = 1 + data_td_count;
    int td_count = status_td_index + 1;
    uint32_t td_status_flags = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT(3) |
                               (low_speed ? UHCI_TD_LS : 0);

    if (UHCI_CONTROL_DMA_TD_OFFSET + td_count * sizeof(uhci_td_t) > PAGE_SIZE) {
        pfree(dma_page_raw);
        return -1;
    }

    memcpy(dma_setup, setup, sizeof(*setup));
    if (length && !data_in) memcpy(dma_data, data, length);

    tds[0].link_ptr = (uint32_t)(tds_phys + sizeof(uhci_td_t));
    tds[0].status = td_status_flags;
    tds[0].token = UHCI_TD_EXPECTED_LENGTH(8) | ((uint32_t)addr << 8) |
                   UHCI_PID_SETUP;
    tds[0].buffer_ptr = (uint32_t)dma_phys;

    uint16_t remaining = length;
    uint16_t offset = 0;
    uint8_t toggle = 1;
    for (int i = 0; i < data_td_count; i++) {
        uint16_t packet = remaining < max_packet ? remaining : max_packet;
        uhci_td_t *td = &tds[1 + i];
        td->link_ptr = (uint32_t)(tds_phys + (2 + i) * sizeof(uhci_td_t));
        td->status = td_status_flags;
        if (data_in && i + 1 < data_td_count) td->status |= UHCI_TD_SPD;
        td->token = UHCI_TD_EXPECTED_LENGTH(packet) |
                    ((uint32_t)toggle << 19) | ((uint32_t)addr << 8) |
                    (data_in ? UHCI_PID_IN : UHCI_PID_OUT);
        td->buffer_ptr = (uint32_t)(dma_phys + UHCI_CONTROL_DMA_DATA_OFFSET + offset);
        remaining -= packet;
        offset += packet;
        toggle ^= 1;
    }

    uhci_td_t *status_td = &tds[status_td_index];
    status_td->link_ptr = (uint32_t)virt_to_phys(ctrl->term_td);
    status_td->status = td_status_flags;
    status_td->token = UHCI_TD_EXPECTED_LENGTH(0) | (1u << 19) |
                       ((uint32_t)addr << 8) |
                       (length && data_in ? UHCI_PID_OUT : UHCI_PID_IN);
    status_td->buffer_ptr = 0;

    __sync_synchronize();
    ctrl->qh->element_link_ptr = (uint32_t)tds_phys;

    int ret = -2;
    bool short_redirected = false;
    for (int elapsed = 0; elapsed < 1000; elapsed++) {
        bool failed = false;
        for (int i = 0; i < td_count; i++) {
            int td_result = uhci_check_td(&tds[i]);
            if (td_result == -2) {
                failed = true;
                break;
            }
        }
        if (failed) break;

        ret = uhci_check_td(status_td);
        if (ret == 0) break;

        if (data_in && !short_redirected) {
            remaining = length;
            for (int i = 0; i + 1 < data_td_count; i++) {
                uint16_t expected = remaining < max_packet ? remaining : max_packet;
                uint32_t status = *(volatile uint32_t *)&tds[1 + i].status;
                if (!(status & UHCI_TD_ACTIVE)) {
                    uint16_t actual = (uint16_t)((status + 1) & UHCI_TD_ACTLEN_MASK);
                    if (actual < expected) {
                        __sync_synchronize();
                        ctrl->qh->element_link_ptr =
                            (uint32_t)(tds_phys + status_td_index * sizeof(uhci_td_t));
                        short_redirected = true;
                        break;
                    }
                }
                remaining -= expected;
            }
        }
        sleep(1);
    }

    int actual_total = 0;
    if (ret == 0 && data_in) {
        remaining = length;
        for (int i = 0; i < data_td_count; i++) {
            uint16_t expected = remaining < max_packet ? remaining : max_packet;
            uint32_t status = *(volatile uint32_t *)&tds[1 + i].status;
            if (status & UHCI_TD_ACTIVE) break;
            uint16_t actual = (uint16_t)((status + 1) & UHCI_TD_ACTLEN_MASK);
            if (actual > expected) actual = expected;
            actual_total += actual;
            remaining -= expected;
            if (actual < expected) break;
        }
        memcpy(data, dma_data, actual_total);
    }

    ctrl->qh->element_link_ptr = (uint32_t)virt_to_phys(ctrl->term_td);
    __sync_synchronize();
    sleep(2);
    pfree(dma_page_raw);
    return ret == 0 ? actual_total : ret;
}

static int uhci_interrupt_transfer(usb_hcd_t *hcd, usb_device_t *dev, uint8_t endpoint, void *data, uint16_t length) {
    if (!hcd || !dev || !data || length == 0 || length > PAGE_SIZE || endpoint > 15)
        return -1;
    uhci_controller_t *ctrl = (uhci_controller_t *)hcd->hcd_data;
    uint8_t addr = dev->address;
    int low_speed = (dev->speed == USB_SPEED_LOW);

    // If there's already a pending transfer, return busy
    if (ctrl->pending_dev != NULL) {
        return -1;
    }

    uhci_td_t *td = ctrl->pending_td;
    memset(ctrl->pending_dma_buf, 0, length);
    td->buffer_ptr = (uint32_t)ctrl->pending_dma_phys;
    td->token = UHCI_TD_EXPECTED_LENGTH(length) |
                ((uint32_t)dev->interrupt_toggle << 19) |
                ((uint32_t)endpoint << 15) | ((uint32_t)addr << 8) |
                UHCI_PID_IN;
    td->status = UHCI_TD_ACTIVE | (low_speed ? UHCI_TD_LS : 0) |
                 UHCI_TD_ERROR_COUNT(3);
    td->link_ptr = UHCI_PTR_TERMINATE;

    __sync_synchronize();
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

static void update_uhci_port(uhci_controller_t *ctrl, int port,
                             uint16_t set, uint16_t clear) {
    uint16_t reg = UHCI_PORTSC1 + (uint16_t)port * 2;
    uint16_t status = inw(ctrl->io_base + reg);
    uint16_t value = status & UHCI_PORT_RW;
    value &= ~clear;
    value |= set;
    outw(ctrl->io_base + reg, value);
}

static void clear_uhci_port_changes(uhci_controller_t *ctrl, int port) {
    uint16_t reg = UHCI_PORTSC1 + (uint16_t)port * 2;
    uint16_t status = inw(ctrl->io_base + reg);
    outw(ctrl->io_base + reg,
         (status & UHCI_PORT_RW) | (status & UHCI_PORT_RWC));
}

static bool reset_uhci_port(uhci_controller_t *ctrl, int port) {
    uint16_t reg = UHCI_PORTSC1 + (uint16_t)port * 2;
    if (!(inw(ctrl->io_base + reg) & UHCI_PORT_CCS)) return false;

    update_uhci_port(ctrl, port, UHCI_PORT_RESET, 0);
    sleep(50);
    update_uhci_port(ctrl, port, 0, UHCI_PORT_RESET);
    sleep(10);
    clear_uhci_port_changes(ctrl, port);

    for (int attempt = 0; attempt < 5; attempt++) {
        uint16_t status = inw(ctrl->io_base + reg);
        if (!(status & UHCI_PORT_CCS)) return false;
        update_uhci_port(ctrl, port, UHCI_PORT_PED, 0);
        sleep(10);
        if (inw(ctrl->io_base + reg) & UHCI_PORT_PED) {
            clear_uhci_port_changes(ctrl, port);
            return true;
        }
    }
    return false;
}

static uint16_t detect_uhci_ports(uhci_controller_t *ctrl) {
    uint16_t ports = 0;
    for (int port = 0; port < 8; port++) {
        uint16_t status = inw(ctrl->io_base + UHCI_PORTSC1 + port * 2);
        if (status == 0xFFFF || !(status & (1u << 7))) break;
        ports++;
    }
    // UHCI requires at least two ports; Linux rejects counts above seven.
    return ports >= 2 && ports <= 7 ? ports : 2;
}

static void remove_uhci_keyboard(uhci_controller_t *ctrl, int port) {
    if (ctrl->pending_dev && ctrl->pending_dev->port_id == port) {
        ctrl->pending_dev = NULL;
        ctrl->pending_buf = NULL;
        ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;
        __sync_synchronize();
    }
    remove_usb_keyboard(&ctrl->hcd, (uint8_t)port);
}

void poll_uhci_ports(void) {
    for (int c = 0; c < uhci_count; c++) {
        uhci_controller_t *ctrl = &uhci_controllers[c];
        if (!ctrl->initialized) continue;

        uint16_t io_base = ctrl->io_base;
        uint16_t hc_status = inw(io_base + UHCI_USBSTS);
        if (hc_status & (UHCI_STS_HCSYSERR | UHCI_STS_HCPROCESS |
                         UHCI_STS_HCHALTED)) {
            log("uhci: controller stopped, status=0x%x\n", hc_status);
            outw(io_base + UHCI_USBCMD, 0);
            ctrl->initialized = 0;
            continue;
        }
        if (hc_status & (UHCI_STS_USBINT | UHCI_STS_USBERRINT |
                         UHCI_STS_RESUME))
            outw(io_base + UHCI_USBSTS, hc_status & 0x1F);

        for (int i = 0; i < ctrl->num_ports; i++) {
            uint16_t port_reg = UHCI_PORTSC1 + (uint16_t)i * 2;
            uint16_t status = inw(io_base + port_reg);

            if (status & UHCI_PORT_RWC) {
                clear_uhci_port_changes(ctrl, i);

                if (status & UHCI_PORT_CCS) {
                    // A PEDC without CSC means the controller disabled the
                    // port after a fault. Resetting loses the USB address, so
                    // discard any old instance before enumerating it again.
                    remove_uhci_keyboard(ctrl, i);

                    // Clear pending state on new connection
                    ctrl->pending_dev = NULL;
                    ctrl->pending_buf = NULL;
                    if (ctrl->intr_qh) ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;

                    sleep(100);
                    if (!reset_uhci_port(ctrl, i)) continue;
                    status = inw(io_base + port_reg);
                    int ls = (status & UHCI_PORT_LSDA) ? 1 : 0;

                    register_usb_hcd(&ctrl->hcd);
                    init_usb_keyboard(&ctrl->hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
                } else {
                    // Device disconnected
                    remove_uhci_keyboard(ctrl, i);
                    if (!ctrl->pending_dev && ctrl->intr_qh)
                        ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;
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
                    uint32_t status = *(volatile uint32_t *)&ctrl->pending_td->status;
                    uint16_t actual = (uint16_t)((status + 1) & UHCI_TD_ACTLEN_MASK);
                    if (actual > ctrl->pending_len) actual = ctrl->pending_len;
                    memset(buf, 0, ctrl->pending_len);
                    memcpy(buf, ctrl->pending_dma_buf, actual);
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
        if (!ctrl->pending_dev && kbd_total > 0) {
            if (ctrl->keyboard_cursor >= kbd_total) ctrl->keyboard_cursor = 0;
            for (int step = 0; step < kbd_total; step++) {
                int k = (ctrl->keyboard_cursor + step) % kbd_total;
                usb_hcd_t *hcd = kbd_list[k].hcd;
                usb_device_t *dev = kbd_list[k].dev;
                uint8_t *buf = kbd_list[k].report_buf_next;
                if (hcd == &ctrl->hcd && dev && buf) {
                    int ret = ctrl->hcd.interrupt_transfer(&ctrl->hcd, dev, kbd_list[k].endpoint_number, buf, 8);
                    if (ret == 0) {
                        ctrl->keyboard_cursor = (k + 1) % kbd_total;
                        break;
                    }
                }
            }
        }
    }
}

bool is_uhci_ready(void) {
    return uhci_count > 0;
}

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
        uint16_t num_ports = ctrl->num_ports ? ctrl->num_ports : 2;

        int port_start = 0;
        int port_end = num_ports;
        if (port_hint >= 0 && port_hint < port_end) {
            port_start = port_hint;
            port_end = port_hint + 1;
        }

        for (int i = port_start; i < port_end; i++) {
            uint16_t port_reg = UHCI_PORTSC1 + (uint16_t)i * 2;
            uint16_t status = inw(io_base + port_reg);

            // Skip completely invalid ports (0x0000 = no port register / invalid I/O)
            if (status == 0x0000) continue;

            // Clear any stale CSC from the ownership transition
            if (status & UHCI_PORT_CSC) {
                clear_uhci_port_changes(ctrl, i);
                status = inw(io_base + port_reg);
            }

            // Skip ports that are already enabled (already enumerated)
            if (status & UHCI_PORT_PED) continue;

            // Wait for CCS to appear — the routing matrix may take a few ms
            if (!(status & UHCI_PORT_CCS)) {
                // Quick check: give the routing matrix just 5ms to settle
                sleep(5);
                status = inw(io_base + port_reg);
                if (status & UHCI_PORT_CSC) {
                    clear_uhci_port_changes(ctrl, i);
                    status = inw(io_base + port_reg);
                }
                if (!(status & UHCI_PORT_CCS)) continue;
            }

            // 100ms debounce per USB 2.0 §9.1.2
            sleep(100);

            ctrl->pending_dev = NULL;
            ctrl->pending_buf = NULL;
            if (ctrl->intr_qh) ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;

            if (!reset_uhci_port(ctrl, i)) continue;
            status = inw(io_base + port_reg);
            int ls = (status & UHCI_PORT_LSDA) ? 1 : 0;

            register_usb_hcd(&ctrl->hcd);
            init_usb_keyboard(&ctrl->hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
        }
    }
}

void init_uhci(pci_device_t *dev) {
    if (!dev || uhci_count >= MAX_UHCI_CONTROLLERS) return;

    uint32_t bar4 = read_pci(dev->bus, dev->dev, dev->func, 0x20);
    if (bar4 == 0xFFFFFFFF || !(bar4 & 1)) {
        log("uhci: BAR4 is not an I/O BAR\n");
        return;
    }
    uint32_t io_base = bar4 & ~0x1FU;

    if (io_base == 0 || io_base > 0xFFE0) {
        log("uhci: invalid io base 0x%x\n", io_base);
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

    uint16_t cmd = read_pci_word(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 0) | (1 << 2);
    write_pci_word(dev->bus, dev->dev, dev->func, 0x04, cmd);

    // UHCI BIOS Handoff: disable USB Legacy Support (SMI generation)
    write_pci_word(dev->bus, dev->dev, dev->func, 0xC0, 0x8F00);

    outw(io_base + UHCI_USBINTR, 0);
    outw(io_base + UHCI_USBCMD, UHCI_CMD_GRESET);
    sleep(50);
    outw(io_base + UHCI_USBCMD, 0);
    sleep(10);

    outw(io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
    bool reset_done = false;
    for (int i = 0; i < 1000; i++) {
        if (!(inw(io_base + UHCI_USBCMD) & UHCI_CMD_HCRESET)) {
            reset_done = true;
            break;
        }
        sleep_us(100);
    }
    if (!reset_done) {
        log("uhci: host controller reset timed out\n");
        return;
    }
    outw(io_base + UHCI_USBINTR, 0);
    outw(io_base + UHCI_USBCMD, 0);

    void *frame_list_raw = pmalloc_dma32();
    if (!frame_list_raw) {
        log("uhci: failed to allocate DMA32 frame list\n");
        return;
    }
    ctrl->frame_list_phys = (uint64_t)frame_list_raw;
    ctrl->frame_list = (uint32_t *)phys_to_virt(ctrl->frame_list_phys);
    ctrl->qh = uhci_alloc_qh();
    ctrl->intr_qh = uhci_alloc_qh();
    ctrl->term_td = uhci_alloc_td();
    ctrl->pending_td = uhci_alloc_td();
    void *pending_dma_raw = pmalloc_dma32();
    if (pending_dma_raw) {
        ctrl->pending_dma_phys = (uint64_t)pending_dma_raw;
        ctrl->pending_dma_buf = (uint8_t *)phys_to_virt(ctrl->pending_dma_phys);
    }

    if (!ctrl->qh || !ctrl->intr_qh || !ctrl->term_td ||
        !ctrl->pending_td || !ctrl->pending_dma_buf) {
        log("uhci: failed to allocate DMA32 schedule memory\n");
        if (ctrl->pending_dma_buf) pfree((void *)ctrl->pending_dma_phys);
        if (ctrl->pending_td) pfree((void *)virt_to_phys(ctrl->pending_td));
        if (ctrl->term_td) pfree((void *)virt_to_phys(ctrl->term_td));
        if (ctrl->intr_qh) pfree((void *)virt_to_phys(ctrl->intr_qh));
        if (ctrl->qh) pfree((void *)virt_to_phys(ctrl->qh));
        pfree(frame_list_raw);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }

    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) {
        ctrl->frame_list[i] = (uint32_t)virt_to_phys(ctrl->intr_qh) |
                              UHCI_PTR_QH;
    }

    ctrl->intr_qh->head_link_ptr = (uint32_t)virt_to_phys(ctrl->qh) | UHCI_PTR_QH;
    ctrl->intr_qh->element_link_ptr = UHCI_PTR_TERMINATE;
    ctrl->qh->head_link_ptr = UHCI_PTR_TERMINATE;

    // Inactive terminating TD required for the Intel PIIX prefetch bug.
    ctrl->term_td->link_ptr = UHCI_PTR_TERMINATE;
    ctrl->term_td->status = 0;
    ctrl->term_td->token = UHCI_TD_EXPECTED_LENGTH(0) | (0x7FU << 8) |
                           UHCI_PID_IN;
    ctrl->term_td->buffer_ptr = 0;
    ctrl->qh->element_link_ptr = (uint32_t)virt_to_phys(ctrl->term_td);

    ctrl->pending_td->link_ptr = UHCI_PTR_TERMINATE;
    ctrl->num_ports = detect_uhci_ports(ctrl);

    __sync_synchronize();
    outl(io_base + UHCI_FLBASEADD, (uint32_t)ctrl->frame_list_phys);
    outw(io_base + UHCI_FRNUM, 0);
    outw(io_base + UHCI_USBSTS, UHCI_STS_USBINT | UHCI_STS_USBERRINT |
                                      UHCI_STS_RESUME | UHCI_STS_HCSYSERR |
                                      UHCI_STS_HCPROCESS);

    outb(io_base + UHCI_SOFMOD, 64);
    outw(io_base + UHCI_USBINTR, 0x0000);
    outw(io_base + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    sleep(2);
    if (inw(io_base + UHCI_USBSTS) & UHCI_STS_HCHALTED) {
        log("uhci: controller failed to start, status=0x%x\n", inw(io_base + UHCI_USBSTS));
        outw(io_base + UHCI_USBCMD, 0);
        pfree((void *)ctrl->pending_dma_phys);
        pfree((void *)virt_to_phys(ctrl->pending_td));
        pfree((void *)virt_to_phys(ctrl->term_td));
        pfree((void *)virt_to_phys(ctrl->intr_qh));
        pfree((void *)virt_to_phys(ctrl->qh));
        pfree(frame_list_raw);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }

    ctrl->hcd.name = "uhci";
    ctrl->hcd.control_transfer = uhci_control_transfer;
    ctrl->hcd.interrupt_transfer = uhci_interrupt_transfer;
    ctrl->hcd.bulk_transfer = uhci_bulk_transfer;
    ctrl->hcd.hcd_data = ctrl;
    ctrl->pending_dev = NULL;
    ctrl->pending_buf = NULL;
    ctrl->initialized = 1;

    log("uhci: initialized uhci\n");
    uhci_count++;

    // Initial port scan - detect already-connected devices
    for (int i = 0; i < ctrl->num_ports; i++) {
        uint16_t port_reg = UHCI_PORTSC1 + (uint16_t)i * 2;
        uint16_t status = inw(io_base + port_reg);

        if (status & UHCI_PORT_CCS) {
            sleep(100);
            if (!reset_uhci_port(ctrl, i)) continue;
            status = inw(io_base + port_reg);
            int ls = (status & UHCI_PORT_LSDA) ? 1 : 0;

            register_usb_hcd(&ctrl->hcd);
            init_usb_keyboard(&ctrl->hcd, ls ? USB_SPEED_LOW : USB_SPEED_FULL, i);
        }
    }
}
