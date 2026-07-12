#include <freestanding/stdint.h>
#include <io/rtl8139.h>
#include <io/io.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <io/net.h>
#include <main/spinlocks.h>
#include <main/log.h>

rtl8139_t rtl8139 = {0};

static spinlock_t rtl_lock = SPINLOCK_INIT;
static bool rtl8139_ready = false;

static uint8_t rtl_read8(uint8_t reg) { return inb(rtl8139.io_base + reg); }
static uint16_t rtl_read16(uint8_t reg) { return inw(rtl8139.io_base + reg); }
static uint32_t rtl_read32(uint8_t reg) { return inl(rtl8139.io_base + reg); }
static void rtl_write8(uint8_t reg, uint8_t v) { outb(rtl8139.io_base + reg, v); }
static void rtl_write16(uint8_t reg, uint16_t v) { outw(rtl8139.io_base + reg, v); }
static void rtl_write32(uint8_t reg, uint32_t v) { outl(rtl8139.io_base + reg, v); }

static void receive_rtl8139(uint64_t *irq) {
    if (!rtl8139_ready) return;
    while (!(rtl_read8(RTL_CMD) & RTL_CMD_BUFE)) {
        uint8_t *buf = rtl8139.rx_buf + rtl8139.rx_offset;
        uint16_t status = *(uint16_t *)(buf + 0);
        uint16_t pkt_len = *(uint16_t *)(buf + 2);

        if (!(status & 0x01) || pkt_len < 14 || pkt_len > 1518) { rtl_write16(RTL_CAPR, rtl_read16(RTL_CBR) - 16); break; }

        uint8_t *pkt = buf + 4;

        spin_unlock_irqrestore(&rtl_lock, *irq);
        // NOTE: Drop lock before calling into net stack (it has its own locks)
        net_rx(pkt, pkt_len);
        spin_lock_irqsave(&rtl_lock, irq);

        rtl8139.rx_offset = (rtl8139.rx_offset + pkt_len + 4 + 3) & ~3;
        rtl8139.rx_offset %= (64 * 1024);
        rtl_write16(RTL_CAPR, (uint16_t)(rtl8139.rx_offset - 16));
    }
}

static void poll_rtl8139(void) {
    if (!rtl8139_ready) return;

    uint64_t irq;
    spin_lock_irqsave(&rtl_lock, &irq);

    uint16_t isr = rtl_read16(RTL_ISR);
    if (!isr) { spin_unlock_irqrestore(&rtl_lock, irq); return; }

    rtl_write16(RTL_ISR, isr);

    if (isr & RTL_INT_ROK) { receive_rtl8139(&irq); }
    if (isr & RTL_INT_RXOVW) {
        // RX overflow...
        rtl8139.rx_offset = 0;
        rtl_write16(RTL_CAPR, 0);
    }
    if (isr & RTL_INT_RER) {
        // RX error...
    }
    if (isr & RTL_INT_TER) {
        // TX error...
    }

    spin_unlock_irqrestore(&rtl_lock, irq);
}

bool send_rtl8139(const void *data, uint16_t len) {
    if (!rtl8139_ready) return false;
    if (len > RTL_TX_BUF_SIZE) return false;

    uint64_t irq;
    spin_lock_irqsave(&rtl_lock, &irq);

    int slot = rtl8139.tx_slot;
    static const uint8_t tsd_regs[4] = {RTL_TSD0, RTL_TSD1, RTL_TSD2, RTL_TSD3};
    static const uint8_t tsad_regs[4] = {RTL_TSAD0, RTL_TSAD1, RTL_TSAD2, RTL_TSAD3};

    int timeout = 100000;
    while (!(rtl_read32(tsd_regs[slot]) & RTL_TSD_OWN) && timeout--) {
        spin_unlock_irqrestore(&rtl_lock, irq);
        io_wait();
        spin_lock_irqsave(&rtl_lock, &irq);
    }
    if (timeout <= 0) { spin_unlock_irqrestore(&rtl_lock, irq); return false; }

    uint16_t send_len = len < 60 ? 60 : len;

    memcpy(rtl8139.tx_buf[slot], data, len);
    if (len < 60) { memset((uint8_t*)rtl8139.tx_buf[slot] + len, 0, 60 - len); }

    rtl_write32(tsad_regs[slot], (uint32_t)virt_to_phys(rtl8139.tx_buf[slot]));
    rtl_write32(tsd_regs[slot], send_len & 0x1FFF);

    rtl8139.tx_slot = (slot + 1) % RTL_TX_SLOTS;

    spin_unlock_irqrestore(&rtl_lock, irq);
    return true;
}

void get_rtl8139_mac(uint8_t mac[6]) { memcpy(mac, rtl8139.mac, 6); }

void init_rtl8139(pci_device_t *dev) {
    if (!dev) return;

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    rtl8139.io_base = (uint16_t)(bar0 & 0xFFFC);

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x07);

    rtl_write8(RTL_CONFIG1, 0x00);

    rtl_write8(RTL_CMD, RTL_CMD_RST);
    while (rtl_read8(RTL_CMD) & RTL_CMD_RST);

    uint32_t mac0 = rtl_read32(RTL_MAC0);
    uint16_t mac4 = rtl_read16(RTL_MAC4);
    rtl8139.mac[0] = (mac0 >> 0) & 0xFF;
    rtl8139.mac[1] = (mac0 >> 8) & 0xFF;
    rtl8139.mac[2] = (mac0 >> 16) & 0xFF;
    rtl8139.mac[3] = (mac0 >> 24) & 0xFF;
    rtl8139.mac[4] = (mac4 >> 0) & 0xFF;
    rtl8139.mac[5] = (mac4 >> 8) & 0xFF;

    uint32_t rx_phys = (uint32_t)virt_to_phys(rtl8139.rx_buf);
    rtl_write32(RTL_RBSTART, rx_phys);
    rtl8139.rx_offset = 0;

    rtl_write8(RTL_CMD, RTL_CMD_RE | RTL_CMD_TE);
    rtl_write32(RTL_RCR, RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_RBLEN_64K | RTL_RCR_WRAP);
    rtl_write32(RTL_TCR, 0x03000700);
    rtl_write16(RTL_IMR, RTL_INT_ROK | RTL_INT_TOK | RTL_INT_RER | RTL_INT_TER | RTL_INT_RXOVW);

    pci_request_irq(dev, poll_rtl8139);

    log("initialized rtl8139");

    rtl8139.tx_slot = 0;
    rtl8139_ready = true;

    static net_device_t net_dev;
    memcpy(net_dev.mac, rtl8139.mac, 6);
    net_dev.send = send_rtl8139;
    net_register_device(&net_dev);
}
