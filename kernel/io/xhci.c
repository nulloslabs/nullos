#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/xhci.h>
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

#define MAX_XHCI_CONTROLLERS 4

static xhci_controller_t xhci_controllers[MAX_XHCI_CONTROLLERS];
static int xhci_controller_count = 0;
static uint64_t xhci_initialized_bars[MAX_XHCI_CONTROLLERS];

// ============================================================================
// MMIO helpers
// ============================================================================
static uint32_t read32_xhci(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}

static void write32_xhci(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(base + offset) = val;
}

static uint8_t read8_xhci(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint8_t*)(base + offset);
}

static void xhci_bios_handoff(xhci_controller_t *ctrl, uint32_t hccparams1) {
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;
    if (!xecp) return;

    volatile uint8_t *ptr = ctrl->cap_regs + (xecp << 2);
    while (1) {
        uint32_t cap = *(volatile uint32_t*)ptr;
        if ((cap & 0xFF) == 0x01) {
            // USB Legacy Support capability: set OS Ownership Semaphore (bit 24)
            *(volatile uint32_t*)ptr = cap | (1u << 24);
            printf("xHCI: Requesting OS ownership from BIOS/UEFI...\n");
            for (int i = 0; i < 1000; i++) {
                if (!(*(volatile uint32_t*)ptr & (1u << 16))) {
                    printf("xHCI: BIOS released ownership.\n");
                    break;
                }
                sleep(1);
            }
            // Disable all SMI enables in USBLEGCTLSTS (DWORD immediately after USBLEGSUP)
            volatile uint32_t *legctlsts = (volatile uint32_t*)(ptr + 4);
            *legctlsts &= ~0x1Fu;
            return;
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (!next) break;
        ptr += (next << 2);
    }
    printf("xHCI: No USBLEGSUP capability found.\n");
}

static void advance_event_ring_xhci(xhci_controller_t *ctrl) {
    ctrl->evt_ring_dequeue++;
    if (ctrl->evt_ring_dequeue >= XHCI_EVT_RING_SIZE) {
        ctrl->evt_ring_dequeue = 0;
        ctrl->evt_ring_cycle ^= 1;
    }

    uint64_t erdp = ctrl->evt_ring_phys + (ctrl->evt_ring_dequeue * sizeof(xhci_trb_t));
    write32_xhci(ctrl->rt_regs, XHCI_RT_ERDP_LO(0), (uint32_t)(erdp & 0xFFFFFFFF) | (1 << 3));
    write32_xhci(ctrl->rt_regs, XHCI_RT_ERDP_HI(0), (uint32_t)(erdp >> 32));
}

// ============================================================================
// Transfer stubs
// ============================================================================
static int control_transfer_xhci(usb_hcd_t *hcd, usb_device_t *dev,
                                  usb_setup_packet_t *setup, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)setup; (void)data; (void)length;
    return -1;
}

static int interrupt_transfer_xhci(usb_hcd_t *hcd, usb_device_t *dev,
                                    uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)endpoint; (void)data; (void)length;
    return -1;
}

static int bulk_transfer_xhci(usb_hcd_t *hcd, usb_device_t *dev,
                               uint8_t endpoint, void *data, uint16_t length) {
    (void)hcd; (void)dev; (void)endpoint; (void)data; (void)length;
    return -1;
}

// ============================================================================
// Port speed string helper
// ============================================================================
static const char* speed_str_xhci(uint32_t speed) {
    switch (speed) {
        case XHCI_SPEED_FULL:  return "full (12 Mbps)";
        case XHCI_SPEED_LOW:   return "low (1.5 Mbps)";
        case XHCI_SPEED_HIGH:  return "high (480 Mbps)";
        case XHCI_SPEED_SUPER: return "super (5 Gbps)";
        default: return "unknown";
    }
}

// ============================================================================
// Ring Management
// ============================================================================
static void send_xhci_command(xhci_controller_t *ctrl, xhci_trb_t *cmd) {
    xhci_trb_t *trb = &ctrl->cmd_ring[ctrl->cmd_ring_enqueue];
    *trb = *cmd;
    trb->control |= (ctrl->cmd_ring_cycle ? XHCI_TRB_CYCLE : 0);

    ctrl->cmd_ring_enqueue++;
    if (ctrl->cmd_ring_enqueue >= XHCI_CMD_RING_SIZE - 1) {
        xhci_trb_t *link = &ctrl->cmd_ring[XHCI_CMD_RING_SIZE - 1];
        if (ctrl->cmd_ring_cycle) {
            link->control |= XHCI_TRB_CYCLE;
        } else {
            link->control &= ~XHCI_TRB_CYCLE;
        }
        ctrl->cmd_ring_enqueue = 0;
        ctrl->cmd_ring_cycle ^= 1;
    }

    write32_xhci((volatile uint8_t*)ctrl->db_regs, 0, 0);
}

static xhci_trb_t* wait_for_event_xhci(xhci_controller_t *ctrl, int expected_type) {
    for (int timeout = 0; timeout < 20000; timeout++) {
        xhci_trb_t *evt = &ctrl->evt_ring[ctrl->evt_ring_dequeue];
        int cycle_state = (evt->control & XHCI_TRB_CYCLE) ? 1 : 0;

        if (cycle_state == ctrl->evt_ring_cycle) {
            int trb_type = (evt->control >> 10) & 0x3F;
            if (trb_type == expected_type) {
                return evt;
            } else {
                advance_event_ring_xhci(ctrl);
            }
        } else {
            sleep(1);
        }
    }
    printf("xHCI: wait_for_event timeout (expected type %d).\n", expected_type);
    return NULL;
}

// ============================================================================
// USB Device Enumerator for xHCI
// ============================================================================
// Context layout (xHCI spec 6.2): stride S = 32 (CSZ=0) or 64 (CSZ=1).
//   Input context buffer:  [0*S]=InputCtrl  [1*S]=Slot  [2*S]=EP0(DCI1)
//                          [3*S]=EP1OUT(DCI2)  [4*S]=EP1IN(DCI3)
//   Output context buffer: [0*S]=Slot  [1*S]=EP0  [2*S]=EP1OUT  [3*S]=EP1IN
// All field writes use byte offsets so they work for both S=32 and S=64.
static void initialize_device_xhci(xhci_controller_t *ctrl, int port_idx, int speed) {
    if (speed == 0) return;
    int S = ctrl->ctx_stride; // 32 or 64

    // enable slot
    xhci_trb_t cmd = {0};
    cmd.control = (9 << 10);
    send_xhci_command(ctrl, &cmd);
    xhci_trb_t *evt = wait_for_event_xhci(ctrl, 33);
    if (!evt) return;
    int slot_id = (evt->control >> 24) & 0xFF;
    advance_event_ring_xhci(ctrl);
    if (slot_id == 0) { printf("xHCI: Failed to get slot ID.\n"); return; }

    for (int k = 0; k < kbd_total; k++) {
        if (kbd_list[k].hcd == &ctrl->hcd && kbd_list[k].dev &&
            kbd_list[k].dev->port_id == port_idx) {
            kbd_list[k].dev->address = slot_id;
            kbd_list[k].dev->hcd_data = ctrl;
            break;
        }
    }

    // allocate output device context
    size_t out_ctx_sz = S * 32;
    uint8_t *out_raw = malloc(out_ctx_sz + 64);
    uint8_t *out_ctx = (uint8_t*)(((uint64_t)out_raw + 63) & ~63ULL);
    memset(out_ctx, 0, out_ctx_sz);
    ctrl->dcbaap[slot_id] = virt_to_phys(out_ctx);
    ctrl->dev_ctx[slot_id] = out_ctx;

    // allocate input context
    size_t in_ctx_sz = S * 33;
    uint8_t *in_raw = malloc(in_ctx_sz + 64);
    uint8_t *ib = (uint8_t*)(((uint64_t)in_raw + 63) & ~63ULL);
    memset(ib, 0, in_ctx_sz);

    // allocate EP0 ring
    xhci_trb_t *ep0_ring = malloc(256 * sizeof(xhci_trb_t) + 64);
    ep0_ring = (xhci_trb_t*)(((uint64_t)ep0_ring + 63) & ~63ULL);
    memset(ep0_ring, 0, 256 * sizeof(xhci_trb_t));

    // address device
    *(uint32_t*)(ib + 4)       = (1u<<0)|(1u<<1);          // add flags: Slot+EP0
    *(uint32_t*)(ib + 1*S + 0) = (1u<<27)|(speed<<20);     // Slot: CE=1, Speed
    *(uint32_t*)(ib + 1*S + 4) = (uint32_t)(port_idx+1)<<16; // Slot: Root Hub Port
    // EP0 context at 2*S (DCI=1)
    *(uint32_t*)(ib + 2*S + 4)  = (3u<<1)|(4u<<3)|(8u<<16);
    *(uint64_t*)(ib + 2*S + 8)  = virt_to_phys(ep0_ring)|1;
    *(uint32_t*)(ib + 2*S + 16) = 8;

    memset(&cmd, 0, sizeof(cmd));
    cmd.param_lo = (uint32_t)(virt_to_phys(ib) & 0xFFFFFFFF);
    cmd.param_hi = (uint32_t)(virt_to_phys(ib) >> 32);
    cmd.control  = (11u<<10)|(slot_id<<24);
    send_xhci_command(ctrl, &cmd);
    evt = wait_for_event_xhci(ctrl, 33);
    if (!evt) return;
    int cc = (evt->status >> 24) & 0xFF;
    advance_event_ring_xhci(ctrl);
    if (cc != 1) { printf("xHCI: Address Device failed (cc=%d).\n", cc); return; }

    // SET_CONFIGURATION and SET_PROTOCOL
    int ep0_enq = 0, ep0_cyc = 1;

    #define EP0_CTRL_XFER(setup_lo, setup_hi) do { \
        ep0_ring[ep0_enq].param_lo  = (setup_lo); \
        ep0_ring[ep0_enq].param_hi  = (setup_hi); \
        ep0_ring[ep0_enq].status    = 8; \
        ep0_ring[ep0_enq].control   = (2u<<10)|(0u<<16)|ep0_cyc|(1u<<6); \
        ep0_ring[ep0_enq+1].param_lo = 0; ep0_ring[ep0_enq+1].param_hi = 0; \
        ep0_ring[ep0_enq+1].status   = 0; \
        ep0_ring[ep0_enq+1].control  = (4u<<10)|(1u<<16)|ep0_cyc|(1u<<5); \
        ep0_enq += 2; \
        write32_xhci((volatile uint8_t*)ctrl->db_regs, slot_id * 4, 1); \
        evt = wait_for_event_xhci(ctrl, 32); \
        if (evt) { cc = (evt->status>>24)&0xFF; advance_event_ring_xhci(ctrl); } \
        else { cc = -1; } \
    } while (0)

    // bmRequestType=0x00 bRequest=0x09 wValue=0x0001 wIndex=0 wLength=0
    EP0_CTRL_XFER(0x00010900u, 0x00000000u);
    if (cc != 1) printf("xHCI: SET_CONFIGURATION failed (cc=%d).\n", cc);

    // bmRequestType=0x21 bRequest=0x0B wValue=0x0000 wIndex=0 wLength=0
    EP0_CTRL_XFER(0x00000B21u, 0x00000000u);
    if (cc != 1) printf("xHCI: SET_PROTOCOL failed (cc=%d).\n", cc);

    #undef EP0_CTRL_XFER

    // configure EP1 IN (DCI=3)
    memset(ib, 0, in_ctx_sz);

    xhci_trb_t *ep1 = malloc(256 * sizeof(xhci_trb_t) + 64);
    ep1 = (xhci_trb_t*)(((uint64_t)ep1 + 63) & ~63ULL);
    memset(ep1, 0, 256 * sizeof(xhci_trb_t));
    ctrl->ep1_rings[slot_id]  = ep1;
    ctrl->ep1_enqueue[slot_id] = 0;
    ctrl->ep1_cycle[slot_id]  = 1;

    // A0 (Slot) + A3 (EP1 IN, DCI=3)
    *(uint32_t*)(ib + 4)       = (1u<<0)|(1u<<3);
    *(uint32_t*)(ib + 1*S + 0) = (3u<<27)|(speed<<20); // Slot: CE=3
    *(uint32_t*)(ib + 1*S + 4) = (uint32_t)(port_idx+1)<<16;
    // EP1 IN at 4*S (DCI=3)
    *(uint32_t*)(ib + 4*S + 0)  = (3u<<16);
    *(uint32_t*)(ib + 4*S + 4)  = (3u<<1)|(7u<<3)|(8u<<16);
    *(uint64_t*)(ib + 4*S + 8)  = virt_to_phys(ep1)|1;
    *(uint32_t*)(ib + 4*S + 16) = 8;

    memset(&cmd, 0, sizeof(cmd));
    cmd.param_lo = (uint32_t)(virt_to_phys(ib) & 0xFFFFFFFF);
    cmd.param_hi = (uint32_t)(virt_to_phys(ib) >> 32);
    cmd.control  = (12u<<10)|(slot_id<<24);
    send_xhci_command(ctrl, &cmd);
    evt = wait_for_event_xhci(ctrl, 33);
    if (!evt) return;
    cc = (evt->status >> 24) & 0xFF;
    advance_event_ring_xhci(ctrl);
    if (cc != 1) { printf("xHCI: Configure EP failed (cc=%d).\n", cc); return; }

    // queue first interrupt TRB
    uint8_t *report_buf = NULL;
    for (int k = 0; k < kbd_total; k++) {
        if (kbd_list[k].hcd == &ctrl->hcd && kbd_list[k].dev &&
            kbd_list[k].dev->port_id == port_idx) {
            report_buf = kbd_list[k].report_buf;
            break;
        }
    }
    if (!report_buf) {
        report_buf = malloc(8);
        if (!report_buf) { printf("xHCI: Failed to alloc report buf.\n"); return; }
        memset(report_buf, 0, 8);
        for (int k = 0; k < kbd_total; k++) {
            if (kbd_list[k].hcd == &ctrl->hcd && kbd_list[k].dev &&
                kbd_list[k].dev->port_id == port_idx) {
                kbd_list[k].report_buf = report_buf; break;
            }
        }
    }

    ep1[0].param_lo = (uint32_t)(virt_to_phys(report_buf) & 0xFFFFFFFF);
    ep1[0].param_hi = (uint32_t)(virt_to_phys(report_buf) >> 32);
    ep1[0].status   = 8;
    ep1[0].control  = (1u<<10)|1|(1u<<5); // Normal TRB, Cycle=1, IOC=1
    ctrl->ep1_enqueue[slot_id] = 1;

    write32_xhci((volatile uint8_t*)ctrl->db_regs, slot_id * 4, 3); // DCI=3 doorbell
    printf("xHCI: Keyboard initialized on port %d (slot %d, S=%d).\n", port_idx+1, slot_id, S);
}

// ============================================================================
// Hot-plug helper: reset and probe a single port
// ============================================================================
static void handle_port_connect_xhci(xhci_controller_t *ctrl, volatile uint8_t *op, int port_idx) {
    uint32_t port_offset = XHCI_OP_PORTSC_BASE + (port_idx * 0x10);
    uint32_t portsc = read32_xhci(op, port_offset);

    uint32_t speed = (portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;

    // Reset port
    write32_xhci(op, port_offset, (portsc & XHCI_PORT_RW_MASK) | XHCI_PORT_PR);
    for (int j = 0; j < 100; j++) {
        portsc = read32_xhci(op, port_offset);
        if (portsc & XHCI_PORT_PRC) break;
        sleep(1);
    }
    write32_xhci(op, port_offset, (portsc & XHCI_PORT_RW_MASK) | XHCI_PORT_PRC);

    // Re-read speed after reset (it's only valid once port is in U0)
    portsc = read32_xhci(op, port_offset);
    speed = (portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
    if (speed == 0) {
        printf("xHCI: Port %d: speed=0 after reset, bailing.\n", port_idx + 1);
        return;
    }

    printf("xHCI: Port %d: speed=%s\n", port_idx + 1, speed_str_xhci(speed));

    uint8_t usb_speed = (speed == XHCI_SPEED_FULL) ? USB_SPEED_FULL :
                        (speed == XHCI_SPEED_LOW)  ? USB_SPEED_LOW  :
                        (speed == XHCI_SPEED_HIGH) ? USB_SPEED_HIGH : USB_SPEED_SUPER;
    init_usb_keyboard(&ctrl->hcd, usb_speed, port_idx);

    initialize_device_xhci(ctrl, port_idx, speed);
}

// ============================================================================
// Hot-plug: poll ports for connect/disconnect changes
// ============================================================================
void poll_xhci_ports(void) {
    for (int c = 0; c < xhci_controller_count; c++) {
        xhci_controller_t *ctrl = &xhci_controllers[c];
        if (!ctrl->initialized) continue;

        volatile uint8_t *op = ctrl->op_regs;

        // Process transfer events first
        while (1) {
            xhci_trb_t *evt = &ctrl->evt_ring[ctrl->evt_ring_dequeue];
            int cycle_state = (evt->control & XHCI_TRB_CYCLE) ? 1 : 0;
            if (cycle_state != ctrl->evt_ring_cycle) break;

            int trb_type = (evt->control >> 10) & 0x3F;
            if (trb_type == 32) { // Transfer Event
                int slot_id = (evt->control >> 24) & 0xFF;
                int ep_id   = (evt->control >> 16) & 0x1F;

                if (ep_id == 3) { // EP1 IN
                    int enq = ctrl->ep1_enqueue[slot_id];
                    if (enq > 0 && enq < 256 && ctrl->ep1_rings[slot_id]) {
                        uint64_t trb_phys = (uint64_t)ctrl->ep1_rings[slot_id][enq - 1].param_lo
                                          | ((uint64_t)ctrl->ep1_rings[slot_id][enq - 1].param_hi << 32);
                        if (trb_phys == 0) {
                            advance_event_ring_xhci(ctrl);
                            continue;
                        }
                        uint8_t *buf = (uint8_t*)phys_to_virt(trb_phys);
                        int ki = -1;
                        for (int k = 0; k < kbd_total; k++) {
                            if (kbd_list[k].hcd == &ctrl->hcd &&
                                kbd_list[k].dev && kbd_list[k].dev->address == slot_id) {
                                ki = k;
                                break;
                            }
                        }

                        if (ki >= 0 && kbd_list[ki].report_buf) {
                            memcpy(kbd_list[ki].report_buf, buf, 8);
                            usb_keyboard_process_report(kbd_list[ki].report_buf, ki);
                        }

                        // Re-queue TRB for continuous polling
                        int cur_enq = ctrl->ep1_enqueue[slot_id];
                        xhci_trb_t *ep1 = ctrl->ep1_rings[slot_id];
                        ep1[cur_enq].param_lo = (uint32_t)(virt_to_phys(buf) & 0xFFFFFFFF);
                        ep1[cur_enq].param_hi = (uint32_t)(virt_to_phys(buf) >> 32);
                        ep1[cur_enq].status   = 8;
                        ep1[cur_enq].control  = (1 << 10) | (ctrl->ep1_cycle[slot_id] ? 1 : 0) | (1 << 5);

                        ctrl->ep1_enqueue[slot_id] = (cur_enq + 1) % 256;
                        if (ctrl->ep1_enqueue[slot_id] == 0)
                            ctrl->ep1_cycle[slot_id] ^= 1;

                        write32_xhci((volatile uint8_t*)ctrl->db_regs, slot_id * 4, 3);
                    }
                }
            }

            advance_event_ring_xhci(ctrl);
        }

        // Process port changes
        uint32_t usbsts = read32_xhci(op, XHCI_OP_USBSTS);
        if (!(usbsts & XHCI_STS_PCD)) continue;

        write32_xhci(op, XHCI_OP_USBSTS, XHCI_STS_PCD);

        for (int i = 0; i < ctrl->max_ports; i++) {
            uint32_t port_offset = XHCI_OP_PORTSC_BASE + (i * 0x10);
            uint32_t portsc = read32_xhci(op, port_offset);

            if (portsc & XHCI_PORT_CSC) {
                write32_xhci(op, port_offset, (portsc & XHCI_PORT_RW_MASK) | XHCI_PORT_CSC);

                if (portsc & XHCI_PORT_CCS) {
                    printf("xHCI: Port %d: Device connected.\n", i + 1);
                    handle_port_connect_xhci(ctrl, op, i);
                } else {
                    printf("xHCI: Port %d: Device disconnected.\n", i + 1);
                    for (int k = 0; k < kbd_total; k++) {
                        if (kbd_list[k].hcd == &ctrl->hcd &&
                            kbd_list[k].dev && kbd_list[k].dev->port_id == i) {
                            int sid = kbd_list[k].dev->address;
                            if (sid > 0 && sid < 256) ctrl->ep1_rings[sid] = NULL;
                            memset(&kbd_list[k], 0, sizeof(kbd_entry_t));
                            kbd_total--;
                            if (k < kbd_total)
                                memmove(&kbd_list[k], &kbd_list[k + 1],
                                        (kbd_total - k) * sizeof(kbd_entry_t));
                            break;
                        }
                    }
                }
            }

            if (portsc & XHCI_PORT_WRC)
                write32_xhci(op, port_offset, (portsc & XHCI_PORT_RW_MASK) | XHCI_PORT_WRC);
            if (portsc & XHCI_PORT_PRC)
                write32_xhci(op, port_offset, (portsc & XHCI_PORT_RW_MASK) | XHCI_PORT_PRC);
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================
void init_xhci(pci_device_t *dev) {
    if (xhci_controller_count >= MAX_XHCI_CONTROLLERS) {
        printf("xHCI: Max controllers reached.\n");
        return;
    }

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint64_t mmio_phys = bar0 & ~0x0FULL;
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);
        mmio_phys |= ((uint64_t)bar1 << 32);
    }

    for (int i = 0; i < xhci_controller_count; i++) {
        if (xhci_initialized_bars[i] == mmio_phys) return;
    }

    if (mmio_phys == 0) {
        printf("xHCI: No MMIO base found.\n");
        return;
    }

    xhci_controller_t *ctrl = &xhci_controllers[xhci_controller_count];
    memset(ctrl, 0, sizeof(xhci_controller_t));

    // ---- ACPI _CRS validation of MMIO base ----
    acpi_resource_list_t res_list = {0};
    if (get_acpi_usb_resources(dev->bus, dev->dev, dev->func, &res_list)) {
        ctrl->acpi_validated = 1;
        printf("xHCI: ACPI _CRS confirmed %d resources.\n", res_list.count);
        for (int r = 0; r < res_list.count; r++) {
            acpi_resource_t *res = &res_list.resources[r];
            if (!res->valid) continue;
            if (res->type == ACPI_RES_MMIO) {
                printf("xHCI:   MMIO range 0x%08lX-0x%08lX\n",
                       (unsigned long)res->base, (unsigned long)(res->base + res->length - 1));
                uint64_t acpi_lo = res->base;
                uint64_t acpi_hi = res->base + res->length - 1;
                if (mmio_phys >= acpi_lo && mmio_phys <= acpi_hi) {
                    printf("xHCI:   PCI BAR MMIO 0x%08lX is within ACPI range -- VALID.\n",
                           (unsigned long)mmio_phys);
                } else {
                    printf("xHCI:   WARNING: PCI BAR MMIO 0x%08lX is OUTSIDE ACPI range!\n",
                           (unsigned long)mmio_phys);
                }
            }
            else if (res->type == ACPI_RES_IRQ) {
                printf("xHCI:   IRQ %u\n", res->irq);
            }
        }
    } else {
        printf("xHCI: No ACPI _CRS match -- using PCI BAR only.\n");
    }
    ctrl->mmio_phys_addr = mmio_phys;

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd);

    volatile uint8_t *cap_regs = (volatile uint8_t *)phys_to_virt(mmio_phys);
    ctrl->cap_regs = cap_regs;

    uint8_t cap_length = read8_xhci(cap_regs, XHCI_CAP_CAPLENGTH);
    ctrl->op_regs = cap_regs + cap_length;

    uint32_t hcsparams1 = read32_xhci(cap_regs, XHCI_CAP_HCSPARAMS1);
    int raw_slots = hcsparams1 & 0xFF;
    ctrl->max_slots = (raw_slots == 0) ? 256 : raw_slots;
    ctrl->max_ports = (hcsparams1 >> 24) & 0xFF;

    uint32_t dboff = read32_xhci(cap_regs, XHCI_CAP_DBOFF);
    ctrl->db_regs = (volatile uint32_t*)(cap_regs + (dboff & ~0x03));

    uint32_t rtsoff = read32_xhci(cap_regs, XHCI_CAP_RTSOFF);
    ctrl->rt_regs = cap_regs + (rtsoff & ~0x1F);

    uint32_t hccparams1 = read32_xhci(cap_regs, XHCI_CAP_HCCPARAMS1);
    xhci_bios_handoff(ctrl, hccparams1);
    ctrl->ctx_stride = (hccparams1 & (1u << 2)) ? 64 : 32;
    printf("xHCI: CSZ=%d (context stride=%d bytes).\n", (hccparams1>>2)&1, ctrl->ctx_stride);

    volatile uint8_t *op = ctrl->op_regs;

    // Halt
    uint32_t usbcmd = read32_xhci(op, XHCI_OP_USBCMD);
    usbcmd &= ~XHCI_CMD_RS;
    write32_xhci(op, XHCI_OP_USBCMD, usbcmd);
    for (int i = 0; i < 100; i++) {
        if (read32_xhci(op, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
        sleep(1);
    }

    // Reset
    write32_xhci(op, XHCI_OP_USBCMD, XHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        uint32_t sts     = read32_xhci(op, XHCI_OP_USBSTS);
        uint32_t cmd_val = read32_xhci(op, XHCI_OP_USBCMD);
        if (!(cmd_val & XHCI_CMD_HCRESET) && !(sts & XHCI_STS_CNR)) break;
        sleep(1);
    }

    write32_xhci(op, XHCI_OP_CONFIG, ctrl->max_slots);

    // DCBAAP
    size_t dcbaap_size = (ctrl->max_slots + 1) * sizeof(uint64_t);
    ctrl->dcbaap = (uint64_t*)malloc(dcbaap_size + 64);
    ctrl->dcbaap = (uint64_t*)(((uint64_t)ctrl->dcbaap + 63) & ~63ULL);
    memset(ctrl->dcbaap, 0, dcbaap_size);
    ctrl->dcbaap_phys = virt_to_phys(ctrl->dcbaap);
    write32_xhci(op, XHCI_OP_DCBAAP_LO, (uint32_t)(ctrl->dcbaap_phys & 0xFFFFFFFF));
    write32_xhci(op, XHCI_OP_DCBAAP_HI, (uint32_t)(ctrl->dcbaap_phys >> 32));

    // Command Ring
    size_t cmd_ring_size = XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t);
    ctrl->cmd_ring = (xhci_trb_t*)malloc(cmd_ring_size + 64);
    ctrl->cmd_ring = (xhci_trb_t*)(((uint64_t)ctrl->cmd_ring + 63) & ~63ULL);
    memset(ctrl->cmd_ring, 0, cmd_ring_size);
    ctrl->cmd_ring_phys = virt_to_phys(ctrl->cmd_ring);
    ctrl->cmd_ring_enqueue = 0;
    ctrl->cmd_ring_cycle = 1;

    xhci_trb_t *link = &ctrl->cmd_ring[XHCI_CMD_RING_SIZE - 1];
    link->param_lo = (uint32_t)(ctrl->cmd_ring_phys & 0xFFFFFFFF);
    link->param_hi = (uint32_t)(ctrl->cmd_ring_phys >> 32);
    link->status   = 0;
    link->control  = XHCI_TRB_LINK | XHCI_TRB_CYCLE | (1 << 1);

    uint64_t crcr = ctrl->cmd_ring_phys | XHCI_TRB_CYCLE;
    write32_xhci(op, XHCI_OP_CRCR_LO, (uint32_t)(crcr & 0xFFFFFFFF));
    write32_xhci(op, XHCI_OP_CRCR_HI, (uint32_t)(crcr >> 32));

    // Event Ring
    size_t evt_ring_size = XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t);
    ctrl->evt_ring = (xhci_trb_t*)malloc(evt_ring_size + 64);
    ctrl->evt_ring = (xhci_trb_t*)(((uint64_t)ctrl->evt_ring + 63) & ~63ULL);
    memset(ctrl->evt_ring, 0, evt_ring_size);
    ctrl->evt_ring_phys   = virt_to_phys(ctrl->evt_ring);
    ctrl->evt_ring_dequeue = 0;
    ctrl->evt_ring_cycle = 1;

    ctrl->erst = (xhci_erst_entry_t*)malloc(sizeof(xhci_erst_entry_t) + 64);
    ctrl->erst = (xhci_erst_entry_t*)(((uint64_t)ctrl->erst + 63) & ~63ULL);
    memset(ctrl->erst, 0, sizeof(xhci_erst_entry_t));
    ctrl->erst_phys = virt_to_phys(ctrl->erst);

    ctrl->erst[0].ring_base = ctrl->evt_ring_phys;
    ctrl->erst[0].ring_size = XHCI_EVT_RING_SIZE;

    volatile uint8_t *rt = ctrl->rt_regs;
    write32_xhci(rt, XHCI_RT_ERSTSZ(0), 1);

    uint64_t erdp = ctrl->evt_ring_phys;
    write32_xhci(rt, XHCI_RT_ERDP_LO(0), (uint32_t)(erdp & 0xFFFFFFFF) | (1 << 3));
    write32_xhci(rt, XHCI_RT_ERDP_HI(0), (uint32_t)(erdp >> 32));

    write32_xhci(rt, XHCI_RT_ERSTBA_LO(0), (uint32_t)(ctrl->erst_phys & 0xFFFFFFFF));
    write32_xhci(rt, XHCI_RT_ERSTBA_HI(0), (uint32_t)(ctrl->erst_phys >> 32));

    write32_xhci(rt, XHCI_RT_IMOD(0), 4000);
    write32_xhci(rt, XHCI_RT_IMAN(0), 0x03);

    usbcmd = read32_xhci(op, XHCI_OP_USBCMD);
    usbcmd |= XHCI_CMD_RS;
    write32_xhci(op, XHCI_OP_USBCMD, usbcmd);
    for (int i = 0; i < 100; i++) {
        if (!(read32_xhci(op, XHCI_OP_USBSTS) & XHCI_STS_HCH)) break;
        sleep(1);
    }

    ctrl->hcd.name           = "xHCI";
    ctrl->hcd.control_transfer   = control_transfer_xhci;
    ctrl->hcd.interrupt_transfer = interrupt_transfer_xhci;
    ctrl->hcd.bulk_transfer      = bulk_transfer_xhci;
    ctrl->hcd.hcd_data           = ctrl;
    ctrl->initialized = 1;

    xhci_initialized_bars[xhci_controller_count] = mmio_phys;
    xhci_controller_count++;

    printf("xHCI: Initialized xHCI.\n");

    for (int i = 0; i < ctrl->max_ports; i++) {
        uint32_t port_offset = XHCI_OP_PORTSC_BASE + (i * 0x10);
        uint32_t portsc = read32_xhci(op, port_offset);
        if (portsc & XHCI_PORT_CCS)
            handle_port_connect_xhci(ctrl, op, i);
    }
}