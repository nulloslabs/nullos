#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/log.h>
#include <main/string.h>
#include <io/time.h>
#include <io/ohci.h>
#include <io/pci.h>
#include <io/usb.h>
#include <io/usb_keyboard.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define OHCI_MAX_CONTROL_DATA 512
#define OHCI_CONTROL_SETUP_OFFSET 0
#define OHCI_CONTROL_DATA_OFFSET 16
#define OHCI_CONTROL_ED_OFFSET 544
#define OHCI_CONTROL_TD_OFFSET 576
#define OHCI_INTERRUPT_ED_OFFSET 0
#define OHCI_INTERRUPT_TD_OFFSET 32
#define OHCI_INTERRUPT_DUMMY_TD_OFFSET 64
#define OHCI_INTERRUPT_DATA_OFFSET 96

_Static_assert(sizeof(ohci_hcca_t) == 256, "OHCI HCCA must be 256 bytes");
_Static_assert(sizeof(ohci_ed_t) == 16, "OHCI ED must be 16 bytes");
_Static_assert(sizeof(ohci_td_t) == 16, "OHCI TD must be 16 bytes");

static ohci_controller_t ohci_controllers[MAX_OHCI_CONTROLLERS];
static int ohci_count;

static uint32_t read_ohci_register(ohci_controller_t *ctrl, uint16_t offset) {
    return *(volatile uint32_t *)(ctrl->registers + offset);
}

static void write_ohci_register(ohci_controller_t *ctrl, uint16_t offset, uint32_t value) {
    *(volatile uint32_t *)(ctrl->registers + offset) = value;
}

static uint8_t get_ohci_td_condition(ohci_td_t *td) {
    return (uint8_t)((td->flags & OHCI_TD_CC) >> 28);
}

static uint16_t get_ohci_td_actual(ohci_td_t *td, uint32_t start, uint16_t length) {
    uint32_t current = td->current_buffer;
    if (length == 0 || current == 0) return length;
    if (current < start || current > start + length) return 0;
    return (uint16_t)(current - start);
}

static uint32_t make_ohci_ed_flags(usb_device_t *dev, uint8_t endpoint, uint16_t max_packet, uint32_t direction) {
    uint32_t flags = (uint32_t)dev->address | ((uint32_t)endpoint << 7) | ((uint32_t)max_packet << 16) | direction;
    if (dev->speed == USB_SPEED_LOW) flags |= OHCI_ED_LOW_SPEED;
    return flags;
}

static void fill_ohci_td(ohci_td_t *td, uint32_t flags, uint32_t buffer, uint16_t length, uint32_t next) {
    td->flags = OHCI_TD_NOT_ACCESSED | flags;
    td->current_buffer = length ? buffer : 0;
    td->next_td = next;
    td->buffer_end = length ? buffer + length - 1 : 0;
}

static void cancel_ohci_interrupt(ohci_controller_t *ctrl) {
    ctrl->interrupt_ed->flags |= OHCI_ED_SKIP;
    __sync_synchronize();
    ctrl->pending_dev = NULL;
    ctrl->pending_buffer = NULL;
    ctrl->pending_length = 0;
}

static void remove_ohci_keyboard(ohci_controller_t *ctrl, uint8_t port) {
    if (ctrl->pending_dev && ctrl->pending_dev->port_id == port) cancel_ohci_interrupt(ctrl);
    remove_usb_keyboard(&ctrl->hcd, port);
}

static void clear_ohci_port_changes(ohci_controller_t *ctrl, uint8_t port, uint32_t status) {
    uint32_t changes = status & OHCI_PORT_CHANGE_MASK;
    if (changes) write_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port), changes);
}

static bool reset_ohci_port(ohci_controller_t *ctrl, uint8_t port) {
    uint16_t start_frame = (uint16_t)read_ohci_register(ctrl, OHCI_FRAME_NUMBER);
    uint16_t end_frame = start_frame + 50;
    int pulses = 0;

    while ((int16_t)((uint16_t)read_ohci_register(ctrl, OHCI_FRAME_NUMBER) - end_frame) < 0 && pulses < 5) {
        uint32_t status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
        if (status == UINT32_MAX || !(status & OHCI_PORT_CCS)) return false;

        bool reset_idle = false;
        for (int elapsed = 0; elapsed < 40; elapsed++) {
            status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
            if (!(status & OHCI_PORT_CCS)) return false;
            if (!(status & OHCI_PORT_PRS)) {
                reset_idle = true;
                break;
            }
            sleep_us(500);
        }
        if (!reset_idle) break;
        if (status & OHCI_PORT_PRSC) write_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port), OHCI_PORT_PRSC);
        write_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port), OHCI_PORT_PRS);
        sleep(10);
        pulses++;
    }

    uint32_t status = 0;
    for (int elapsed = 0; elapsed < 100; elapsed++) {
        status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
        if (status == UINT32_MAX || !(status & OHCI_PORT_CCS)) return false;
        if (!(status & OHCI_PORT_PRS)) {
            clear_ohci_port_changes(ctrl, port, status);
            if (status & OHCI_PORT_PES) return true;
        }
        sleep(1);
    }
    return false;
}

static int perform_ohci_control_transfer(usb_hcd_t *hcd, usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t length) {
    if (!hcd || !dev || !setup || length > OHCI_MAX_CONTROL_DATA) return -1;
    if (length && !data) return -1;

    ohci_controller_t *ctrl = (ohci_controller_t *)hcd->hcd_data;
    if (!ctrl || !ctrl->initialized || ctrl->control_busy) return -1;
    uint16_t max_packet = dev->max_packet_size;
    if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64) return -1;

    ctrl->control_busy = true;
    uint8_t *page = ctrl->control_page;
    uint64_t page_phys = ctrl->control_page_phys;
    usb_setup_packet_t *dma_setup = (usb_setup_packet_t *)(page + OHCI_CONTROL_SETUP_OFFSET);
    uint8_t *dma_data = page + OHCI_CONTROL_DATA_OFFSET;
    ohci_td_t *tds = (ohci_td_t *)(page + OHCI_CONTROL_TD_OFFSET);
    uint32_t tds_phys = (uint32_t)(page_phys + OHCI_CONTROL_TD_OFFSET);
    bool data_in = (setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0;
    int data_td_count = length ? (length + max_packet - 1) / max_packet : 0;
    int status_index = 1 + data_td_count;
    int dummy_index = status_index + 1;
    uint32_t dummy_phys = tds_phys + (uint32_t)dummy_index * sizeof(ohci_td_t);

    memset(page, 0, PAGE_SIZE);
    memcpy(dma_setup, setup, sizeof(*setup));
    if (length && !data_in) memcpy(dma_data, data, length);

    fill_ohci_td(&tds[0], OHCI_TD_DATA0 | OHCI_TD_SETUP | OHCI_TD_DELAY(7), (uint32_t)page_phys, sizeof(*setup), tds_phys + sizeof(ohci_td_t));
    uint16_t remaining = length;
    uint16_t offset = 0;
    uint8_t toggle = 1;
    for (int i = 0; i < data_td_count; i++) {
        uint16_t packet = remaining < max_packet ? remaining : max_packet;
        uint32_t flags = toggle ? OHCI_TD_DATA1 : OHCI_TD_DATA0;
        flags |= data_in ? OHCI_TD_IN : OHCI_TD_OUT;
        if (data_in && i + 1 == data_td_count) flags |= OHCI_TD_ROUND;
        flags |= OHCI_TD_DELAY(7);
        fill_ohci_td(&tds[1 + i], flags, (uint32_t)(page_phys + OHCI_CONTROL_DATA_OFFSET + offset), packet, tds_phys + (uint32_t)(2 + i) * sizeof(ohci_td_t));
        remaining -= packet;
        offset += packet;
        toggle ^= 1;
    }

    uint32_t status_flags = OHCI_TD_DATA1 | (data_in && length ? OHCI_TD_OUT : OHCI_TD_IN);
    fill_ohci_td(&tds[status_index], status_flags, 0, 0, dummy_phys);
    memset(&tds[dummy_index], 0, sizeof(ohci_td_t));

    uint32_t ed_flags = make_ohci_ed_flags(dev, 0, max_packet, 0);
    ctrl->control_ed->flags = ed_flags | OHCI_ED_SKIP;
    ctrl->control_ed->tail_pointer = dummy_phys;
    ctrl->control_ed->head_pointer = tds_phys;
    ctrl->control_ed->next_ed = 0;
    __sync_synchronize();
    ctrl->control_ed->flags = ed_flags;
    write_ohci_register(ctrl, OHCI_COMMAND_STATUS, OHCI_COMMAND_CLF);

    int result = -2;
    for (int elapsed = 0; elapsed < 1000; elapsed++) {
        bool failed = false;
        for (int i = 0; i <= status_index; i++) {
            uint8_t condition = get_ohci_td_condition(&tds[i]);
            if (condition != 0 && condition != 15) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        if (get_ohci_td_condition(&tds[status_index]) == 0) {
            result = 0;
            break;
        }
        sleep(1);
    }

    int actual_total = 0;
    if (result == 0 && data_in) {
        remaining = length;
        offset = 0;
        for (int i = 0; i < data_td_count; i++) {
            uint16_t packet = remaining < max_packet ? remaining : max_packet;
            uint32_t start = (uint32_t)(page_phys + OHCI_CONTROL_DATA_OFFSET + offset);
            uint16_t actual = get_ohci_td_actual(&tds[1 + i], start, packet);
            actual_total += actual;
            remaining -= packet;
            offset += packet;
            if (actual < packet) break;
        }
        memcpy(data, dma_data, actual_total);
    }

    ctrl->control_ed->flags = ed_flags | OHCI_ED_SKIP;
    __sync_synchronize();
    sleep(2);
    ctrl->control_ed->head_pointer = dummy_phys;
    ctrl->control_ed->tail_pointer = dummy_phys;
    ctrl->control_busy = false;
    return result == 0 ? actual_total : result;
}

static int submit_ohci_interrupt_transfer(usb_hcd_t *hcd, usb_device_t *dev, uint8_t endpoint, void *data, uint16_t length) {
    if (!hcd || !dev || !data || endpoint == 0 || endpoint > 15) return -1;
    if (length == 0 || length > PAGE_SIZE - OHCI_INTERRUPT_DATA_OFFSET) return -1;

    ohci_controller_t *ctrl = (ohci_controller_t *)hcd->hcd_data;
    if (!ctrl || !ctrl->initialized || ctrl->pending_dev) return -1;
    uint16_t max_packet = dev->interrupt_max_packet;
    if (max_packet < length || max_packet > 64) return -1;

    uint32_t td_phys = (uint32_t)(ctrl->interrupt_page_phys + OHCI_INTERRUPT_TD_OFFSET);
    uint32_t dummy_phys = (uint32_t)(ctrl->interrupt_page_phys + OHCI_INTERRUPT_DUMMY_TD_OFFSET);
    uint32_t buffer_phys = (uint32_t)(ctrl->interrupt_page_phys + OHCI_INTERRUPT_DATA_OFFSET);
    uint32_t td_flags = OHCI_TD_IN | (dev->interrupt_toggle ? OHCI_TD_DATA1 : OHCI_TD_DATA0);
    uint32_t ed_flags = make_ohci_ed_flags(dev, endpoint, max_packet, OHCI_ED_IN);

    ctrl->interrupt_ed->flags = ed_flags | OHCI_ED_SKIP;
    memset(ctrl->interrupt_buffer, 0, length);
    fill_ohci_td(ctrl->interrupt_td, td_flags, buffer_phys, length, dummy_phys);
    memset(ctrl->interrupt_dummy_td, 0, sizeof(ohci_td_t));
    ctrl->interrupt_ed->tail_pointer = dummy_phys;
    ctrl->interrupt_ed->head_pointer = td_phys;
    ctrl->interrupt_ed->next_ed = 0;
    ctrl->pending_dev = dev;
    ctrl->pending_buffer = (uint8_t *)data;
    ctrl->pending_length = length;
    __sync_synchronize();
    ctrl->interrupt_ed->flags = ed_flags;
    return 0;
}

static int perform_ohci_bulk_transfer(usb_hcd_t *hcd, usb_device_t *dev, uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd;
    (void)dev;
    (void)endpoint;
    (void)data;
    (void)length;
    return -1;
}

static void finish_ohci_interrupt(ohci_controller_t *ctrl) {
    if (!ctrl->pending_dev) return;
    uint8_t condition = get_ohci_td_condition(ctrl->interrupt_td);
    if (condition == 15) return;

    usb_device_t *dev = ctrl->pending_dev;
    uint8_t *buffer = ctrl->pending_buffer;
    uint16_t length = ctrl->pending_length;
    ctrl->interrupt_ed->flags |= OHCI_ED_SKIP;
    __sync_synchronize();

    if (condition == 0 && buffer) {
        uint32_t start = (uint32_t)(ctrl->interrupt_page_phys + OHCI_INTERRUPT_DATA_OFFSET);
        uint16_t actual = get_ohci_td_actual(ctrl->interrupt_td, start, length);
        memset(buffer, 0, length);
        memcpy(buffer, ctrl->interrupt_buffer, actual);
        dev->interrupt_toggle ^= 1;
        int index = kbd_find_index(dev);
        if (index >= 0 && index < kbd_total) {
            usb_keyboard_process_report(buffer, index);
            uint8_t *temporary = kbd_list[index].report_buf;
            kbd_list[index].report_buf = kbd_list[index].report_buf_next;
            kbd_list[index].report_buf_next = temporary;
        }
    }

    ctrl->pending_dev = NULL;
    ctrl->pending_buffer = NULL;
    ctrl->pending_length = 0;
}

static void arm_ohci_keyboard(ohci_controller_t *ctrl) {
    if (ctrl->pending_dev || kbd_total == 0) return;
    if (ctrl->keyboard_cursor >= kbd_total) ctrl->keyboard_cursor = 0;
    for (int step = 0; step < kbd_total; step++) {
        int i = (ctrl->keyboard_cursor + step) % kbd_total;
        if (kbd_list[i].hcd != &ctrl->hcd || !kbd_list[i].dev || !kbd_list[i].report_buf_next) continue;
        int result = submit_ohci_interrupt_transfer(&ctrl->hcd, kbd_list[i].dev, kbd_list[i].endpoint_number, kbd_list[i].report_buf_next, 8);
        if (result == 0) {
            ctrl->keyboard_cursor = (i + 1) % kbd_total;
            return;
        }
    }
}

static void enumerate_ohci_port(ohci_controller_t *ctrl, uint8_t port) {
    sleep(100);
    uint32_t status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
    if (!(status & OHCI_PORT_CCS)) return;
    if (!reset_ohci_port(ctrl, port)) return;
    status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
    uint8_t speed = status & OHCI_PORT_LSDA ? USB_SPEED_LOW : USB_SPEED_FULL;
    init_usb_keyboard(&ctrl->hcd, speed, port);
}

static void scan_ohci_ports(ohci_controller_t *ctrl) {
    for (uint8_t port = 0; port < ctrl->num_ports; port++) {
        uint32_t status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
        if (status == UINT32_MAX) continue;
        clear_ohci_port_changes(ctrl, port, status);
        if (!(status & OHCI_PORT_CCS)) continue;
        ctrl->present_ports |= (uint16_t)(1u << port);
        enumerate_ohci_port(ctrl, port);
    }
}

static bool take_ohci_ownership(ohci_controller_t *ctrl) {
    uint32_t control = read_ohci_register(ctrl, OHCI_CONTROL);
    if (control & OHCI_CONTROL_IR) {
        write_ohci_register(ctrl, OHCI_INTERRUPT_ENABLE, OHCI_INTERRUPT_OC);
        write_ohci_register(ctrl, OHCI_COMMAND_STATUS, OHCI_COMMAND_OCR);
        bool released = false;
        for (int elapsed = 0; elapsed < 500; elapsed++) {
            if (!(read_ohci_register(ctrl, OHCI_CONTROL) & OHCI_CONTROL_IR)) {
                released = true;
                break;
            }
            sleep(10);
        }
        if (!released) {
            log("ohci: firmware ownership request timed out\n");
            return false;
        }
        control = read_ohci_register(ctrl, OHCI_CONTROL);
    }

    write_ohci_register(ctrl, OHCI_INTERRUPT_DISABLE, OHCI_INTERRUPT_MIE);
    uint32_t state = control & OHCI_CONTROL_HCFS;
    uint32_t preserved = control & OHCI_CONTROL_RWC;
    if (state == OHCI_USB_OPERATIONAL) {
        write_ohci_register(ctrl, OHCI_CONTROL, preserved | OHCI_USB_RESET);
        sleep(50);
    } else if (state == OHCI_USB_SUSPEND || state == OHCI_USB_RESUME) {
        write_ohci_register(ctrl, OHCI_CONTROL, preserved | OHCI_USB_RESUME);
        sleep(10);
        write_ohci_register(ctrl, OHCI_CONTROL, preserved | OHCI_USB_RESET);
        sleep(50);
    }
    return true;
}

static bool allocate_ohci_resources(ohci_controller_t *ctrl) {
    void *hcca_raw = pmalloc_dma32();
    void *control_raw = pmalloc_dma32();
    void *interrupt_raw = pmalloc_dma32();
    if (!hcca_raw || !control_raw || !interrupt_raw) {
        if (hcca_raw) pfree(hcca_raw);
        if (control_raw) pfree(control_raw);
        if (interrupt_raw) pfree(interrupt_raw);
        return false;
    }

    ctrl->hcca_phys = (uint64_t)hcca_raw;
    ctrl->control_page_phys = (uint64_t)control_raw;
    ctrl->interrupt_page_phys = (uint64_t)interrupt_raw;
    ctrl->hcca = (ohci_hcca_t *)phys_to_virt(ctrl->hcca_phys);
    ctrl->control_page = (uint8_t *)phys_to_virt(ctrl->control_page_phys);
    ctrl->interrupt_page = (uint8_t *)phys_to_virt(ctrl->interrupt_page_phys);
    memset(ctrl->hcca, 0, PAGE_SIZE);
    memset(ctrl->control_page, 0, PAGE_SIZE);
    memset(ctrl->interrupt_page, 0, PAGE_SIZE);

    ctrl->control_ed = (ohci_ed_t *)(ctrl->control_page + OHCI_CONTROL_ED_OFFSET);
    ctrl->interrupt_ed = (ohci_ed_t *)(ctrl->interrupt_page + OHCI_INTERRUPT_ED_OFFSET);
    ctrl->interrupt_td = (ohci_td_t *)(ctrl->interrupt_page + OHCI_INTERRUPT_TD_OFFSET);
    ctrl->interrupt_dummy_td = (ohci_td_t *)(ctrl->interrupt_page + OHCI_INTERRUPT_DUMMY_TD_OFFSET);
    ctrl->interrupt_buffer = ctrl->interrupt_page + OHCI_INTERRUPT_DATA_OFFSET;
    return true;
}

static void release_ohci_resources(ohci_controller_t *ctrl) {
    if (ctrl->hcca) pfree((void *)ctrl->hcca_phys);
    if (ctrl->control_page) pfree((void *)ctrl->control_page_phys);
    if (ctrl->interrupt_page) pfree((void *)ctrl->interrupt_page_phys);
    if (ctrl->mmio_mapping) vunmap_mmio(ctrl->mmio_mapping, ctrl->mmio_pages);
}

static bool start_ohci_controller(ohci_controller_t *ctrl) {
    write_ohci_register(ctrl, OHCI_COMMAND_STATUS, OHCI_COMMAND_HCR);
    bool reset_done = false;
    for (int elapsed = 0; elapsed < 100; elapsed++) {
        if (!(read_ohci_register(ctrl, OHCI_COMMAND_STATUS) & OHCI_COMMAND_HCR)) {
            reset_done = true;
            break;
        }
        sleep_us(10);
    }
    if (!reset_done) return false;

    memset(ctrl->hcca, 0, PAGE_SIZE);
    memset(ctrl->control_page, 0, PAGE_SIZE);
    memset(ctrl->interrupt_page, 0, PAGE_SIZE);
    uint32_t control_ed_phys = (uint32_t)(ctrl->control_page_phys + OHCI_CONTROL_ED_OFFSET);
    uint32_t interrupt_ed_phys = (uint32_t)(ctrl->interrupt_page_phys + OHCI_INTERRUPT_ED_OFFSET);
    ctrl->control_ed->flags = OHCI_ED_SKIP;
    ctrl->interrupt_ed->flags = OHCI_ED_SKIP;
    for (int i = 0; i < 32; i++) ctrl->hcca->interrupt_table[i] = interrupt_ed_phys;

    uint32_t old_interval = read_ohci_register(ctrl, OHCI_FRAME_INTERVAL);
    uint32_t interval = old_interval & 0x3FFFu;
    if (interval < 10000 || interval > 14000) interval = OHCI_FRAME_INTERVAL_VALUE;
    uint32_t largest_packet = (6u * (interval - 210u)) / 7u;
    uint32_t frame_interval = ((old_interval ^ (1u << 31)) & (1u << 31)) | (largest_packet << 16) | interval;

    write_ohci_register(ctrl, OHCI_CONTROL_HEAD_ED, control_ed_phys);
    write_ohci_register(ctrl, OHCI_BULK_HEAD_ED, 0);
    write_ohci_register(ctrl, OHCI_HCCA, (uint32_t)ctrl->hcca_phys);
    write_ohci_register(ctrl, OHCI_FRAME_INTERVAL, frame_interval);
    write_ohci_register(ctrl, OHCI_PERIODIC_START, (9u * interval) / 10u);
    write_ohci_register(ctrl, OHCI_LOW_SPEED_THRESHOLD, OHCI_LOW_SPEED_THRESHOLD_VALUE);
    write_ohci_register(ctrl, OHCI_INTERRUPT_STATUS, UINT32_MAX);
    __sync_synchronize();

    uint32_t preserved = read_ohci_register(ctrl, OHCI_CONTROL) & OHCI_CONTROL_RWC;
    write_ohci_register(ctrl, OHCI_CONTROL, preserved | OHCI_CONTROL_PLE | OHCI_CONTROL_CLE | OHCI_USB_OPERATIONAL);
    if ((read_ohci_register(ctrl, OHCI_CONTROL) & OHCI_CONTROL_HCFS) != OHCI_USB_OPERATIONAL) return false;
    if (!(read_ohci_register(ctrl, OHCI_FRAME_INTERVAL) & 0x3FFF0000u)) return false;
    if (!read_ohci_register(ctrl, OHCI_PERIODIC_START)) return false;

    uint32_t descriptor_a = read_ohci_register(ctrl, OHCI_RH_DESCRIPTOR_A);
    if (!(descriptor_a & OHCI_RH_A_NPS)) {
        if (descriptor_a & OHCI_RH_A_PSM) {
            for (uint8_t port = 0; port < ctrl->num_ports; port++) write_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port), OHCI_PORT_PPS);
        } else {
            write_ohci_register(ctrl, OHCI_RH_STATUS, OHCI_RH_STATUS_LPSC);
        }
        uint32_t power_delay = ((descriptor_a & OHCI_RH_A_POTPGT) >> 24) * 2u;
        if (power_delay) sleep(power_delay);
    }
    return true;
}

static void recover_ohci_controller(ohci_controller_t *ctrl) {
    cancel_ohci_interrupt(ctrl);
    for (uint8_t port = 0; port < ctrl->num_ports; port++) remove_usb_keyboard(&ctrl->hcd, port);
    ctrl->present_ports = 0;
    ctrl->control_busy = false;
    ctrl->initialized = false;
    if (!start_ohci_controller(ctrl)) {
        log("ohci: controller recovery failed\n");
        write_ohci_register(ctrl, OHCI_CONTROL, OHCI_USB_RESET);
        return;
    }
    ctrl->initialized = true;
    log("ohci: controller recovered\n");
    scan_ohci_ports(ctrl);
}

bool is_ohci_ready(void) {
    return ohci_count > 0;
}

void poll_ohci_ports(void) {
    for (int index = 0; index < ohci_count; index++) {
        ohci_controller_t *ctrl = &ohci_controllers[index];
        if (!ctrl->initialized) continue;

        uint32_t interrupts = read_ohci_register(ctrl, OHCI_INTERRUPT_STATUS);
        if (interrupts & OHCI_INTERRUPT_UE) {
            log("ohci: unrecoverable controller error\n");
            recover_ohci_controller(ctrl);
            continue;
        }
        if (interrupts) write_ohci_register(ctrl, OHCI_INTERRUPT_STATUS, interrupts);

        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            uint32_t status = read_ohci_register(ctrl, OHCI_RH_PORT_STATUS(port));
            if (status == UINT32_MAX) continue;
            uint16_t bit = (uint16_t)(1u << port);
            bool was_present = (ctrl->present_ports & bit) != 0;
            bool is_present = (status & OHCI_PORT_CCS) != 0;
            bool needs_reprobe = (status & (OHCI_PORT_CSC | OHCI_PORT_PESC)) != 0;
            clear_ohci_port_changes(ctrl, port, status);

            if (!is_present) {
                if (was_present) {
                    remove_ohci_keyboard(ctrl, port);
                    ctrl->present_ports &= (uint16_t)~bit;
                }
                continue;
            }
            if (!was_present || needs_reprobe) {
                remove_ohci_keyboard(ctrl, port);
                ctrl->present_ports |= bit;
                enumerate_ohci_port(ctrl, port);
            }
        }

        finish_ohci_interrupt(ctrl);
        arm_ohci_keyboard(ctrl);
    }
}

void init_ohci(pci_device_t *dev) {
    if (!dev || ohci_count >= MAX_OHCI_CONTROLLERS) return;
    set_pci_d0(dev);

    uint32_t bar_low = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    if (bar_low == UINT32_MAX || (bar_low & 1u)) {
        log("ohci: BAR0 is not a memory BAR\n");
        return;
    }
    uint32_t bar_type = (bar_low >> 1) & 3u;
    if (bar_type != 0 && bar_type != 2) {
        log("ohci: unsupported BAR0 type\n");
        return;
    }
    uint64_t mmio_phys = bar_low & 0xFFFFFFF0u;
    if (bar_type == 2) mmio_phys |= (uint64_t)read_pci(dev->bus, dev->dev, dev->func, 0x14) << 32;
    if (!mmio_phys) {
        log("ohci: invalid BAR0 address\n");
        return;
    }

    for (int i = 0; i < ohci_count; i++) {
        if (ohci_controllers[i].mmio_phys == mmio_phys) return;
    }

    uint64_t page_phys = mmio_phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint16_t page_offset = (uint16_t)(mmio_phys - page_phys);
    uint8_t mmio_pages = (uint8_t)((page_offset + 0x100 + PAGE_SIZE - 1) / PAGE_SIZE);
    void *mapping = vmap_mmio(page_phys, mmio_pages);
    if (!mapping) {
        log("ohci: failed to map BAR0\n");
        return;
    }

    ohci_controller_t *ctrl = &ohci_controllers[ohci_count];
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->mmio_mapping = mapping;
    ctrl->mmio_phys = mmio_phys;
    ctrl->mmio_pages = mmio_pages;
    ctrl->registers = (volatile uint8_t *)mapping + page_offset;

    uint16_t pci_command = read_pci_word(dev->bus, dev->dev, dev->func, 0x04);
    write_pci_word(dev->bus, dev->dev, dev->func, 0x04, pci_command | (1u << 1) | (1u << 2));
    uint8_t revision = (uint8_t)read_ohci_register(ctrl, OHCI_REVISION);
    if ((revision & 0xF0u) != 0x10u) {
        log("ohci: unsupported revision 0x%x\n", revision);
        release_ohci_resources(ctrl);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }
    if (!take_ohci_ownership(ctrl)) {
        release_ohci_resources(ctrl);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }

    uint32_t descriptor_a = read_ohci_register(ctrl, OHCI_RH_DESCRIPTOR_A);
    ctrl->num_ports = (uint8_t)(descriptor_a & OHCI_RH_A_NDP);
    if (ctrl->num_ports == 0 || ctrl->num_ports > OHCI_MAX_PORTS) {
        log("ohci: invalid root hub port count %u\n", ctrl->num_ports);
        release_ohci_resources(ctrl);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }
    if (!allocate_ohci_resources(ctrl)) {
        log("ohci: failed to allocate DMA32 schedule memory\n");
        release_ohci_resources(ctrl);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }
    if (!start_ohci_controller(ctrl)) {
        log("ohci: failed to start controller\n");
        release_ohci_resources(ctrl);
        memset(ctrl, 0, sizeof(*ctrl));
        return;
    }

    ctrl->hcd.name = "ohci";
    ctrl->hcd.control_transfer = perform_ohci_control_transfer;
    ctrl->hcd.interrupt_transfer = submit_ohci_interrupt_transfer;
    ctrl->hcd.bulk_transfer = perform_ohci_bulk_transfer;
    ctrl->hcd.hcd_data = ctrl;
    ctrl->initialized = true;
    register_usb_hcd(&ctrl->hcd);
    log("ohci: initialized ohci\n");
    ohci_count++;
    scan_ohci_ports(ctrl);
}
