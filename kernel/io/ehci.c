#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/log.h>
#include <main/string.h>
#include <io/ehci.h>
#include <io/ohci.h>
#include <io/pci.h>
#include <io/time.h>
#include <io/uhci.h>
#include <io/usb.h>
#include <io/usb_bot.h>
#include <io/usb_keyboard.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

_Static_assert(sizeof(ehci_qtd_t) == 32, "EHCI qTD must be 32 bytes");
_Static_assert(sizeof(ehci_qh_t) == 64, "EHCI QH must be 64 bytes");
_Static_assert(EHCI_XFER_DATA_OFFSET + EHCI_MAX_BULK_DATA <= PAGE_SIZE, "EHCI dma page layout must fit");
_Static_assert(EHCI_INTERRUPT_DATA_OFFSET + EHCI_MAX_INTERRUPT_DATA <= PAGE_SIZE, "ehci interrupt dma range must fit");

static ehci_controller_t ehci_controllers[MAX_EHCI_CONTROLLERS];
static int ehci_count;

static uint32_t read_ehci_cap(ehci_controller_t *ctrl, uint16_t offset) {
    return *(volatile uint32_t *)(ctrl->cap_registers + offset);
}

static uint32_t read_ehci_register(ehci_controller_t *ctrl, uint16_t offset) {
    return *(volatile uint32_t *)(ctrl->op_registers + offset);
}

static void write_ehci_register(ehci_controller_t *ctrl, uint16_t offset, uint32_t value) {
    *(volatile uint32_t *)(ctrl->op_registers + offset) = value;
}

static uint32_t read_ehci_portsc(ehci_controller_t *ctrl, uint8_t port) {
    return read_ehci_register(ctrl, EHCI_OP_PORTSC(port));
}

static void write_ehci_portsc(ehci_controller_t *ctrl, uint8_t port, uint32_t value) {
    write_ehci_register(ctrl, EHCI_OP_PORTSC(port), value);
}

// qemu replaces the ro bits (ccs etc) with the written value, always write the full readback with masks
static void write_ehci_portsc_masked(ehci_controller_t *ctrl, uint8_t port, uint32_t status, uint32_t set, uint32_t clear) {
    write_ehci_portsc(ctrl, port, (status | set) & ~clear);
}

static void fill_ehci_qtd(ehci_qtd_t *qtd, uint32_t token, uint64_t buffer_phys, uint32_t length) {
    qtd->next_qtd = EHCI_QTD_T;
    qtd->alt_next_qtd = EHCI_QTD_T;
    // bits 12-14 of a written token are CPAGE, the error counter only exists on readback
    qtd->token = token | ((length & 0x7FFFu) << 16) | EHCI_QTD_ACTIVE;
    for (int i = 0; i < EHCI_QTD_MAX_PAGES; i++) {
        if (length <= (uint32_t)i * 4096u) { qtd->buffer[i] = 0; continue; }
        uint32_t value = (uint32_t)((buffer_phys + (uint64_t)i * 4096u) & EHCI_QTD_BUF_BASE);
        if (i == 0) value |= (uint32_t)(buffer_phys & EHCI_QTD_BUF_OFFSET);
        qtd->buffer[i] = value;
    }
}

static void deactivate_ehci_qh(ehci_qh_t *qh) {
    qh->current_qtd = EHCI_QTD_T;
    qh->next_qtd = EHCI_QTD_T;
    qh->alt_next_qtd = EHCI_QTD_T;
    qh->token = 0;
    for (int i = 0; i < EHCI_QTD_MAX_PAGES; i++) qh->buffer[i] = 0;
    __sync_synchronize();
}

static void configure_ehci_qh(ehci_qh_t *qh, usb_device_t *dev, uint8_t endpoint, uint16_t max_packet, bool head) {
    uint32_t characteristics = (uint32_t)(dev->address & EHCI_QH_ADDR_MASK);
    characteristics |= ((uint32_t)(endpoint & 0xF) << EHCI_QH_EP_SHIFT);
    characteristics |= ((uint32_t)(max_packet & 0x7FF) << EHCI_QH_MAX_PACKET_SHIFT);
    if (dev->speed == USB_SPEED_LOW) characteristics |= EHCI_QH_SPEED_LOW;
    if (dev->speed == USB_SPEED_HIGH) characteristics |= EHCI_QH_SPEED_HIGH;
    if (head) characteristics |= EHCI_QH_HEAD;
    qh->characteristics = characteristics;
    qh->capabilities = EHCI_QH_MULT_ONE;
    __sync_synchronize();
}

static void activate_ehci_qh(ehci_qh_t *qh, ehci_qtd_t *qtd, uint64_t qtd_phys) {
    qh->current_qtd = (uint32_t)qtd_phys;
    qh->next_qtd = qtd->next_qtd;
    qh->alt_next_qtd = qtd->alt_next_qtd;
    qh->token = qtd->token;
    for (int i = 0; i < EHCI_QTD_MAX_PAGES; i++) qh->buffer[i] = qtd->buffer[i];
    __sync_synchronize();
}

static bool wait_ehci_qtd(ehci_controller_t *ctrl, ehci_qtd_t *qtd, uint32_t timeout_ms) {
    for (int elapsed = 0; elapsed < (int)timeout_ms; elapsed++) {
        uint32_t token = qtd->token;
        if (token & (EHCI_QTD_HALTED | EHCI_QTD_XACTERR | EHCI_QTD_BUFERR | EHCI_QTD_BABBLE)) return false;
        if (!(token & EHCI_QTD_ACTIVE)) return true;
        if (read_ehci_register(ctrl, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) return false;
        sleep(1);
    }
    return false;
}

static void kick_ehci_async(ehci_controller_t *ctrl) {
    uint32_t command = read_ehci_register(ctrl, EHCI_OP_USBCMD);
    if (!(command & EHCI_CMD_RUN)) {
        log("ehci: controller stopped unexpectedly\n");
        ctrl->initialized = false;
        return;
    }
    if (!(command & EHCI_CMD_ASE)) {
        write_ehci_register(ctrl, EHCI_OP_USBCMD, command | EHCI_CMD_ASE);
        for (int elapsed = 0; elapsed < 100; elapsed++) {
            if (read_ehci_register(ctrl, EHCI_OP_USBSTS) & EHCI_STS_ASS) break;
            sleep(1);
        }
    }
    // the schedule walks on its own every frame, the doorbell only speeds that up
    write_ehci_register(ctrl, EHCI_OP_USBCMD, read_ehci_register(ctrl, EHCI_OP_USBCMD) | EHCI_CMD_IAAD);
    for (int elapsed = 0; elapsed < EHCI_IAA_TIMEOUT_MS; elapsed++) {
        if (read_ehci_register(ctrl, EHCI_OP_USBSTS) & EHCI_STS_IAA) break;
        sleep(1);
    }
    write_ehci_register(ctrl, EHCI_OP_USBSTS, EHCI_STS_IAA);
    __sync_synchronize();
}

static void enable_ehci_periodic(ehci_controller_t *ctrl) {
    uint32_t command = read_ehci_register(ctrl, EHCI_OP_USBCMD);
    if (command & EHCI_CMD_PSE) return;
    write_ehci_register(ctrl, EHCI_OP_USBCMD, command | EHCI_CMD_PSE);
    for (int elapsed = 0; elapsed < EHCI_PSS_TIMEOUT_MS; elapsed++) {
        if (read_ehci_register(ctrl, EHCI_OP_USBSTS) & EHCI_STS_PSS) break;
        sleep(1);
    }
}

static int perform_ehci_control_transfer(usb_hcd_t *hcd, usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t length) {
    if (!hcd || !dev || !setup || length > EHCI_MAX_CONTROL_DATA) return -1;
    if (length && !data) return -1;
    ehci_controller_t *ctrl = (ehci_controller_t *)hcd->hcd_data;
    if (!ctrl || !ctrl->initialized || ctrl->control_busy) return -1;
    uint16_t max_packet = dev->max_packet_size;
    if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64) return -1;
    ctrl->control_busy = true;
    uint8_t *dma_data = ctrl->dma_page + EHCI_XFER_DATA_OFFSET;
    uint64_t data_phys = ctrl->dma_page_phys + EHCI_XFER_DATA_OFFSET;
    uint64_t qtd_phys = ctrl->dma_page_phys + EHCI_XFER_QTD_OFFSET;
    ehci_qtd_t *qtds = (ehci_qtd_t *)(ctrl->dma_page + EHCI_XFER_QTD_OFFSET);
    usb_setup_packet_t *dma_setup = (usb_setup_packet_t *)(ctrl->dma_page + EHCI_XFER_SETUP_OFFSET);
    bool data_in = (setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0;
    memset(ctrl->dma_page + EHCI_XFER_QTD_OFFSET, 0, PAGE_SIZE - EHCI_XFER_QTD_OFFSET);
    memcpy(dma_setup, setup, sizeof(*setup));
    if (length && !data_in) memcpy(dma_data, data, length);
    fill_ehci_qtd(&qtds[0], EHCI_QTD_PID_SETUP << EHCI_QTD_PID_SHIFT, ctrl->dma_page_phys + EHCI_XFER_SETUP_OFFSET, sizeof(*setup));
    if (length) fill_ehci_qtd(&qtds[1], (data_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) << EHCI_QTD_PID_SHIFT | EHCI_QTD_TOGGLE, data_phys, length);
    fill_ehci_qtd(&qtds[2], (data_in && length ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN) << EHCI_QTD_PID_SHIFT | EHCI_QTD_TOGGLE, 0, 0);
    uint32_t second_qtd = (uint32_t)(length ? qtd_phys + sizeof(ehci_qtd_t) : qtd_phys + 2 * sizeof(ehci_qtd_t));
    qtds[0].next_qtd = second_qtd & EHCI_QTD_ADDR_MASK;
    if (length) qtds[1].next_qtd = (uint32_t)(qtd_phys + 2 * sizeof(ehci_qtd_t)) & EHCI_QTD_ADDR_MASK;
    __sync_synchronize();
    configure_ehci_qh(ctrl->control_qh, dev, 0, max_packet, true);
    activate_ehci_qh(ctrl->control_qh, &qtds[0], qtd_phys);
    kick_ehci_async(ctrl);
    bool done = wait_ehci_qtd(ctrl, &qtds[2], EHCI_CONTROL_TIMEOUT_MS);
    int actual = 0;
    if (done && data_in && length) {
        uint16_t remaining = (uint16_t)((qtds[1].token & EHCI_QTD_BYTES) >> 16);
        actual = (int)length - (int)remaining;
        if (actual < 0) actual = 0;
        if (actual > length) actual = length;
        if (actual > 0) memcpy(data, dma_data, (size_t)actual);
    }
    deactivate_ehci_qh(ctrl->control_qh);
    ctrl->control_busy = false;
    if (!done) { log("ehci: control transfer failed\n"); return -1; }
    return actual;
}

static int perform_ehci_bulk_transfer(usb_hcd_t *hcd, usb_device_t *dev, uint8_t endpoint, void *data, uint16_t length) {
    if (!hcd || !dev || !data || length == 0 || length > EHCI_MAX_BULK_DATA) return -1;
    bool dir_in = (endpoint & 0x80) != 0;
    uint16_t max_packet = dir_in ? dev->bulk_in_max_packet : dev->bulk_out_max_packet;
    if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64 && max_packet != 512) return -1;
    ehci_controller_t *ctrl = (ehci_controller_t *)hcd->hcd_data;
    if (!ctrl || !ctrl->initialized || ctrl->bulk_busy) return -1;
    ctrl->bulk_busy = true;
    uint8_t *dma_data = ctrl->dma_page + EHCI_XFER_DATA_OFFSET;
    uint64_t data_phys = ctrl->dma_page_phys + EHCI_XFER_DATA_OFFSET;
    ehci_qtd_t *qtd = (ehci_qtd_t *)(ctrl->dma_page + EHCI_XFER_QTD_OFFSET);
    uint64_t qtd_phys = ctrl->dma_page_phys + EHCI_XFER_QTD_OFFSET;
    uint8_t start_toggle = dir_in ? (dev->bulk_in_toggle & 1) : (dev->bulk_out_toggle & 1);
    memset(dma_data, 0, length);
    if (!dir_in) memcpy(dma_data, data, length);
    uint32_t token = (dir_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) << EHCI_QTD_PID_SHIFT;
    if (start_toggle) token |= EHCI_QTD_TOGGLE;
    fill_ehci_qtd(qtd, token, data_phys, length);
    __sync_synchronize();
    configure_ehci_qh(ctrl->bulk_qh, dev, endpoint & 0x0F, max_packet, false);
    activate_ehci_qh(ctrl->bulk_qh, qtd, qtd_phys);
    kick_ehci_async(ctrl);
    bool done = wait_ehci_qtd(ctrl, qtd, EHCI_BULK_TIMEOUT_MS);
    int actual = 0;
    if (done) {
        uint16_t remaining = (uint16_t)((qtd->token & EHCI_QTD_BYTES) >> 16);
        actual = (int)length - (int)remaining;
        if (actual < 0) actual = 0;
        if (actual > length) actual = length;
        if (dir_in && actual > 0) memcpy(data, dma_data, (size_t)actual);
        uint32_t packets = actual ? (uint32_t)((actual + max_packet - 1) / max_packet) : 1;
        uint8_t end_toggle = (uint8_t)((start_toggle + packets) & 1);
        if (dir_in) dev->bulk_in_toggle = end_toggle;
        else dev->bulk_out_toggle = end_toggle;
    }
    deactivate_ehci_qh(ctrl->bulk_qh);
    ctrl->bulk_busy = false;
    if (!done) { log("ehci: bulk transfer failed\n"); return -1; }
    return actual;
}

static void cancel_ehci_interrupt(ehci_controller_t *ctrl) {
    if (!ctrl->interrupt_busy) return;
    ctrl->interrupt_qh->current_qtd = EHCI_QTD_T;
    ctrl->interrupt_qh->next_qtd = EHCI_QTD_T;
    ctrl->interrupt_qh->alt_next_qtd = EHCI_QTD_T;
    __sync_synchronize();
    ctrl->interrupt_dev = NULL;
    ctrl->interrupt_buf = NULL;
    ctrl->interrupt_len = 0;
    ctrl->interrupt_busy = false;
}

static int perform_ehci_interrupt_transfer(usb_hcd_t *hcd, usb_device_t *dev, uint8_t endpoint, void *data, uint16_t length) {
    if (!hcd || !dev || !data || length == 0 || length > EHCI_MAX_INTERRUPT_DATA) return -1;
    if (dev->interrupt_max_packet == 0) return -1;
    ehci_controller_t *ctrl = (ehci_controller_t *)hcd->hcd_data;
    if (!ctrl || !ctrl->initialized || ctrl->interrupt_busy) return -1;
    ehci_qh_t *qh = ctrl->interrupt_qh;
    ehci_qtd_t *qtd = ctrl->interrupt_qtd;
    uint8_t *dma_data = ctrl->dma_page + EHCI_INTERRUPT_DATA_OFFSET;
    uint64_t data_phys = ctrl->dma_page_phys + EHCI_INTERRUPT_DATA_OFFSET;
    uint32_t token = EHCI_QTD_PID_IN << EHCI_QTD_PID_SHIFT;
    if (dev->interrupt_toggle & 1) token |= EHCI_QTD_TOGGLE;
    enable_ehci_periodic(ctrl);
    memset(dma_data, 0, length);
    fill_ehci_qtd(qtd, token, data_phys, length);
    configure_ehci_qh(qh, dev, endpoint & 0x0F, dev->interrupt_max_packet, false);
    qh->capabilities = EHCI_QH_MULT_ONE | EHCI_QH_SMASK_ALL;
    __sync_synchronize();
    activate_ehci_qh(qh, qtd, ctrl->interrupt_qtd_phys);
    ctrl->interrupt_dev = dev;
    ctrl->interrupt_buf = data;
    ctrl->interrupt_len = length;
    ctrl->interrupt_busy = true;
    return 0;
}

static void finish_ehci_interrupt(ehci_controller_t *ctrl) {
    ehci_qtd_t *qtd = ctrl->interrupt_qtd;
    usb_device_t *dev = ctrl->interrupt_dev;
    uint8_t *buf = ctrl->interrupt_buf;
    uint16_t length = ctrl->interrupt_len;
    uint32_t token = qtd->token;
    ctrl->interrupt_qh->current_qtd = EHCI_QTD_T;
    __sync_synchronize();
    ctrl->interrupt_dev = NULL;
    ctrl->interrupt_buf = NULL;
    ctrl->interrupt_len = 0;
    ctrl->interrupt_busy = false;
    if (!dev || !buf || !length) return;
    if (token & (EHCI_QTD_HALTED | EHCI_QTD_XACTERR | EHCI_QTD_BUFERR | EHCI_QTD_BABBLE)) return;
    int actual = (int)length - (int)((token & EHCI_QTD_BYTES) >> 16);
    if (actual < 0) return;
    if (actual > (int)length) actual = (int)length;
    memset(buf, 0, length);
    if (actual > 0) memcpy(buf, ctrl->dma_page + EHCI_INTERRUPT_DATA_OFFSET, (size_t)actual);
    dev->interrupt_toggle ^= 1;
    int ki = kbd_find_index(dev);
    if (ki < 0 || ki >= kbd_total) return;
    usb_keyboard_process_report(buf, ki);
    uint8_t *temp = kbd_list[ki].report_buf;
    kbd_list[ki].report_buf = kbd_list[ki].report_buf_next;
    kbd_list[ki].report_buf_next = temp;
}

static bool check_keyboard_claim(usb_hcd_t *hcd, uint8_t port) {
    for (int i = 0; i < kbd_total; i++) { if (kbd_list[i].hcd == hcd && kbd_list[i].dev && kbd_list[i].dev->port_id == port) return true; }
    return false;
}

static void arm_ehci_keyboard(ehci_controller_t *ctrl) {
    if (ctrl->interrupt_busy || kbd_total == 0) return;
    if (ctrl->keyboard_cursor >= kbd_total) ctrl->keyboard_cursor = 0;
    for (int step = 0; step < kbd_total; step++) {
        int k = (ctrl->keyboard_cursor + step) % kbd_total;
        usb_hcd_t *hcd = kbd_list[k].hcd;
        usb_device_t *dev = kbd_list[k].dev;
        uint8_t *buf = kbd_list[k].report_buf_next;
        if (hcd != &ctrl->hcd || !dev || !buf) continue;
        if (ctrl->hcd.interrupt_transfer(&ctrl->hcd, dev, kbd_list[k].endpoint_number, buf, 8) == 0) {
            ctrl->keyboard_cursor = (k + 1) % kbd_total;
            return;
        }
    }
}

// ehci silicon cannot drive fs/ls devices itself, but qemu can, so only
// release the port to a companion when one of those drivers is actually up
static bool has_companion_controllers(void) {
    if (is_uhci_ready()) return true;
    if (is_ohci_ready()) return true;
    return false;
}

static bool reset_ehci_port(ehci_controller_t *ctrl, uint8_t port) {
    uint32_t status = read_ehci_portsc(ctrl, port);
    if (!(status & EHCI_PORT_CCS)) return false;
    bool companion = has_companion_controllers();
    if ((status & EHCI_PORT_LINE) != 0 && companion) {
        write_ehci_portsc_masked(ctrl, port, status, EHCI_PORT_OWNER, 0);
        return false;
    }
    write_ehci_portsc_masked(ctrl, port, status, EHCI_PORT_PR, 0);
    sleep(EHCI_PORT_RESET_HOLD_MS);
    status = read_ehci_portsc(ctrl, port);
    write_ehci_portsc_masked(ctrl, port, status, 0, EHCI_PORT_PR | EHCI_PORT_CHANGES);
    for (int elapsed = 0; elapsed < EHCI_PORT_RESET_HANDSHAKE_US / 100; elapsed++) {
        if (!(read_ehci_portsc(ctrl, port) & EHCI_PORT_PR)) break;
        sleep_us(100);
    }
    sleep(20);
    status = read_ehci_portsc(ctrl, port);
    if (!(status & EHCI_PORT_CCS)) return false;
    if (status & EHCI_PORT_OWNER) return false;
    if ((status & EHCI_PORT_LINE) != 0) return true;
    if (!(status & EHCI_PORT_PED)) {
        if (companion) write_ehci_portsc_masked(ctrl, port, status, EHCI_PORT_OWNER, 0);
        return false;
    }
    return true;
}

static void enumerate_ehci_port(ehci_controller_t *ctrl, uint8_t port) {
    sleep(100);
    if (!reset_ehci_port(ctrl, port)) return;
    uint32_t status = read_ehci_portsc(ctrl, port);
    if (!(status & EHCI_PORT_CCS)) return;
    uint8_t speed = USB_SPEED_FULL;
    uint32_t line = status & EHCI_PORT_LINE;
    if (line == EHCI_PORT_LINE_LS) speed = USB_SPEED_LOW;
    if (line == 0) speed = USB_SPEED_HIGH;
    if (speed == USB_SPEED_HIGH && !(status & EHCI_PORT_PED)) return;
    init_usb_keyboard(&ctrl->hcd, speed, port);
    if (check_keyboard_claim(&ctrl->hcd, port)) return;
    if (!reset_ehci_port(ctrl, port)) return;
    sleep(100);
    init_usb_bot(&ctrl->hcd, speed, port);
}

static void scan_ehci_ports(ehci_controller_t *ctrl) {
    for (uint8_t port = 0; port < ctrl->num_ports; port++) {
        uint32_t status = read_ehci_portsc(ctrl, port);
        if (status == UINT32_MAX) continue;
        if (!(status & EHCI_PORT_CCS)) continue;
        if (status & EHCI_PORT_OWNER) continue;
        write_ehci_portsc(ctrl, port, status);
        ctrl->present_ports |= (uint16_t)(1u << port);
        enumerate_ehci_port(ctrl, port);
    }
}

void poll_ehci_ports(void) {
    for (int i = 0; i < ehci_count; i++) {
        ehci_controller_t *ctrl = &ehci_controllers[i];
        if (!ctrl->initialized) continue;
        if (ctrl->interrupt_busy) {
            uint32_t token = ctrl->interrupt_qtd->token;
            if (!(token & EHCI_QTD_ACTIVE)) finish_ehci_interrupt(ctrl);
        }
        if (!ctrl->interrupt_busy) arm_ehci_keyboard(ctrl);
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            uint32_t status = read_ehci_portsc(ctrl, port);
            if (status == UINT32_MAX) continue;
            if (status & EHCI_PORT_OWNER) continue;
            bool was_present = (ctrl->present_ports & (1u << port)) != 0;
            if (status & EHCI_PORT_CHANGES) write_ehci_portsc(ctrl, port, status);
            if (!(status & EHCI_PORT_CCS)) {
                if (was_present) {
                    if (ctrl->interrupt_dev && ctrl->interrupt_dev->port_id == port) cancel_ehci_interrupt(ctrl);
                    remove_usb_keyboard(&ctrl->hcd, port);
                    remove_usb_bot(&ctrl->hcd, port);
                    ctrl->present_ports &= (uint16_t)~(1u << port);
                }
                continue;
            }
            if (was_present) continue;
            ctrl->present_ports |= (uint16_t)(1u << port);
            enumerate_ehci_port(ctrl, port);
        }
    }
}

static void take_ehci_ownership(ehci_controller_t *ctrl) {
    uint32_t hcc = read_ehci_cap(ctrl, EHCI_CAP_HCCPARAMS);
    uint8_t ecp = (uint8_t)((hcc & EHCI_HCC_ECP) >> 8);
    if (!ecp) return;
    volatile uint32_t *legsup = (volatile uint32_t *)(ctrl->cap_registers + ecp);
    volatile uint32_t *legctl = (volatile uint32_t *)(ctrl->cap_registers + ecp + 4);
    bool attempted = false;
    uint32_t value = *legsup;
    if (value & EHCI_LEGSUP_BIOS) {
        attempted = true;
        *legsup = value | EHCI_LEGSUP_OS;
        int msec = 1000;
        while ((*legsup & EHCI_LEGSUP_BIOS) && msec > 0) { sleep(10); msec -= 10; }
        if (*legsup & EHCI_LEGSUP_BIOS) { log("ehci: bios handoff failed\n"); *legsup = *legsup & 0xFF00FFFFu; }
    }
    *legctl = 0;
    if (attempted) write_ehci_register(ctrl, EHCI_OP_CONFIGFLAG, 0);
}

static bool allocate_ehci_resources(ehci_controller_t *ctrl) {
    void *dma_raw = pmalloc_dma32();
    void *frame_raw = pmalloc_dma32();
    if (!dma_raw || !frame_raw) { if (dma_raw) pfree(dma_raw); if (frame_raw) pfree(frame_raw); return false; }
    ctrl->dma_page_phys = (uint64_t)dma_raw;
    ctrl->frame_list_phys = (uint64_t)frame_raw;
    ctrl->dma_page = (uint8_t *)phys_to_virt(ctrl->dma_page_phys);
    ctrl->frame_list = (uint8_t *)phys_to_virt(ctrl->frame_list_phys);
    memset(ctrl->dma_page, 0, PAGE_SIZE);
    memset(ctrl->frame_list, 0, PAGE_SIZE);
    ctrl->control_qh = (ehci_qh_t *)(ctrl->dma_page + EHCI_CONTROL_QH_OFFSET);
    ctrl->bulk_qh = (ehci_qh_t *)(ctrl->dma_page + EHCI_BULK_QH_OFFSET);
    ctrl->control_qh_phys = ctrl->dma_page_phys + EHCI_CONTROL_QH_OFFSET;
    ctrl->bulk_qh_phys = ctrl->dma_page_phys + EHCI_BULK_QH_OFFSET;
    ctrl->interrupt_qh = (ehci_qh_t *)(ctrl->dma_page + EHCI_INTERRUPT_QH_OFFSET);
    ctrl->interrupt_qtd = (ehci_qtd_t *)(ctrl->dma_page + EHCI_INTERRUPT_QTD_OFFSET);
    ctrl->interrupt_qh_phys = ctrl->dma_page_phys + EHCI_INTERRUPT_QH_OFFSET;
    ctrl->interrupt_qtd_phys = ctrl->dma_page_phys + EHCI_INTERRUPT_QTD_OFFSET;
    ctrl->control_busy = false;
    ctrl->bulk_busy = false;
    return true;
}

static void release_ehci_resources(ehci_controller_t *ctrl) {
    if (ctrl->dma_page) pfree((void *)ctrl->dma_page_phys);
    if (ctrl->frame_list) pfree((void *)ctrl->frame_list_phys);
    if (ctrl->mmio_mapping) vunmap_mmio(ctrl->mmio_mapping, ctrl->mmio_pages);
}

static bool start_ehci_controller(ehci_controller_t *ctrl) {
    uint32_t command = read_ehci_register(ctrl, EHCI_OP_USBCMD);
    if (command & EHCI_CMD_RUN) {
        write_ehci_register(ctrl, EHCI_OP_USBCMD, command & ~EHCI_CMD_RUN);
        bool halted = false;
        for (int elapsed = 0; elapsed < 2000; elapsed++) {
            if (read_ehci_register(ctrl, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) { halted = true; break; }
            sleep(1);
        }
        if (!halted) { log("ehci: controller did not halt\n"); return false; }
    }
    command = read_ehci_register(ctrl, EHCI_OP_USBCMD);
    write_ehci_register(ctrl, EHCI_OP_USBCMD, command | EHCI_CMD_HCRESET);
    bool reset_done = false;
    for (int elapsed = 0; elapsed < EHCI_CMD_RESET_TIMEOUT_MS; elapsed++) {
        if (!(read_ehci_register(ctrl, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) { reset_done = true; break; }
        sleep(1);
    }
    if (!reset_done) { log("ehci: controller reset timed out\n"); return false; }
    uint32_t *frame = (uint32_t *)ctrl->frame_list;
    for (int i = 0; i < EHCI_FRAME_LIST_ENTRIES; i++) frame[i] = (uint32_t)ctrl->interrupt_qh_phys | EHCI_QH_TYPE_QH;
    deactivate_ehci_qh(ctrl->control_qh);
    deactivate_ehci_qh(ctrl->bulk_qh);
    deactivate_ehci_qh(ctrl->interrupt_qh);
    // periodic qhs must never present an empty s-mask to the schedule
    ctrl->interrupt_qh->capabilities = EHCI_QH_MULT_ONE | EHCI_QH_SMASK_ALL;
    ctrl->control_qh->horizontal_link = (uint32_t)ctrl->bulk_qh_phys | EHCI_QH_TYPE_QH;
    ctrl->bulk_qh->horizontal_link = (uint32_t)ctrl->control_qh_phys | EHCI_QH_TYPE_QH;
    __sync_synchronize();
    write_ehci_register(ctrl, EHCI_OP_USBINTR, 0);
    write_ehci_register(ctrl, EHCI_OP_FRINDEX, 0);
    write_ehci_register(ctrl, EHCI_OP_CTRLDSSEGMENT, 0);
    write_ehci_register(ctrl, EHCI_OP_PERIODICLISTBASE, (uint32_t)ctrl->frame_list_phys);
    write_ehci_register(ctrl, EHCI_OP_ASYNCLISTADDR, (uint32_t)ctrl->control_qh_phys);
    write_ehci_register(ctrl, EHCI_OP_USBSTS, UINT32_MAX);
    write_ehci_register(ctrl, EHCI_OP_USBCMD, EHCI_CMD_RUN | EHCI_CMD_ITC(1));
    (void)read_ehci_register(ctrl, EHCI_OP_USBCMD);
    sleep(5);
    bool running = false;
    for (int elapsed = 0; elapsed < EHCI_RUN_TIMEOUT_MS; elapsed++) {
        if (!(read_ehci_register(ctrl, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED)) { running = true; break; }
        sleep(1);
    }
    if (!running) { log("ehci: controller refused to start\n"); return false; }
    write_ehci_register(ctrl, EHCI_OP_CONFIGFLAG, EHCI_FLAG_CF);
    return true;
}

static void power_ehci_ports(ehci_controller_t *ctrl) {
    uint32_t hcsp = read_ehci_cap(ctrl, EHCI_CAP_HCSPARAMS);
    if (!(hcsp & EHCI_HCS_PPC)) return;
    for (uint8_t port = 0; port < ctrl->num_ports; port++) {
        uint32_t status = read_ehci_portsc(ctrl, port);
        write_ehci_portsc_masked(ctrl, port, status, EHCI_PORT_PP, 0);
    }
    uint32_t delay = ((hcsp & EHCI_HCS_POTPGT) >> 20) * 2u;
    if (delay) sleep(delay);
}

void init_ehci(pci_device_t *dev) {
    if (!dev || ehci_count >= MAX_EHCI_CONTROLLERS) return;
    set_pci_d0(dev);
    uint32_t bar_low = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    if (bar_low == UINT32_MAX || (bar_low & 1u)) { log("ehci: bar0 is not a memory bar\n"); return; }
    uint32_t bar_type = (bar_low >> 1) & 3u;
    if (bar_type != 0 && bar_type != 2) { log("ehci: unsupported bar0 type\n"); return; }
    uint64_t mmio_phys = bar_low & 0xFFFFFFF0u;
    if (bar_type == 2) mmio_phys |= (uint64_t)read_pci(dev->bus, dev->dev, dev->func, 0x14) << 32;
    if (!mmio_phys) { log("ehci: invalid bar0 address\n"); return; }
    for (int i = 0; i < ehci_count; i++) { if (ehci_controllers[i].mmio_phys == mmio_phys) return; }
    uint64_t page_phys = mmio_phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint16_t page_offset = (uint16_t)(mmio_phys - page_phys);
    uint8_t mmio_pages = (uint8_t)((page_offset + 0x100 + PAGE_SIZE - 1) / PAGE_SIZE);
    void *mapping = vmap_mmio(page_phys, mmio_pages);
    if (!mapping) { log("ehci: failed to map bar0\n"); return; }
    ehci_controller_t *ctrl = &ehci_controllers[ehci_count];
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->mmio_mapping = mapping;
    ctrl->mmio_phys = mmio_phys;
    ctrl->mmio_pages = mmio_pages;
    ctrl->cap_registers = (volatile uint8_t *)mapping + page_offset;
    ctrl->op_registers = ctrl->cap_registers + (read_ehci_cap(ctrl, EHCI_CAP_CAPLENGTH) & 0xFF);
    uint16_t pci_command = read_pci_word(dev->bus, dev->dev, dev->func, 0x04);
    write_pci_word(dev->bus, dev->dev, dev->func, 0x04, pci_command | (1u << 1) | (1u << 2));
    uint32_t hcsp = read_ehci_cap(ctrl, EHCI_CAP_HCSPARAMS);
    ctrl->num_ports = (uint8_t)(hcsp & EHCI_HCS_N_PORTS);
    if (ctrl->num_ports == 0 || ctrl->num_ports > EHCI_MAX_PORTS) {
        log("ehci: invalid root hub\n");
        goto fail_ctrl;
    }
    take_ehci_ownership(ctrl);
    if (!allocate_ehci_resources(ctrl)) {
        log("ehci: failed to allocate dma32 schedule memory\n");
        goto fail_ctrl;
    }
    if (!start_ehci_controller(ctrl)) {
        goto fail_ctrl;
    }
    power_ehci_ports(ctrl);
    ctrl->hcd.name = "ehci";
    ctrl->hcd.control_transfer = perform_ehci_control_transfer;
    ctrl->hcd.bulk_transfer = perform_ehci_bulk_transfer;
    ctrl->hcd.interrupt_transfer = perform_ehci_interrupt_transfer;
    ctrl->hcd.hcd_data = ctrl;
    ctrl->initialized = true;
    register_usb_hcd(&ctrl->hcd);
    ehci_count++;
    scan_ehci_ports(ctrl);
    log("ehci: initialized ehci\n");
    return;
fail_ctrl:
    release_ehci_resources(ctrl);
    memset(ctrl, 0, sizeof(*ctrl));
}
