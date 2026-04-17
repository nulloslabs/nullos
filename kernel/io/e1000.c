#include <io/e1000.h>
#include <io/io.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <io/net.h>
#include <io/terminal.h>
#include <main/spinlock.h>

static uint8_t mac_addr[6];
static volatile uint8_t *e1000_mmio;

static e1000_rx_desc_t *rx_descs;
static e1000_tx_desc_t *tx_descs;

static uint8_t *rx_buf[E1000_NUM_RX_DESC];
static uint8_t *tx_buf[E1000_NUM_TX_DESC];

static uint16_t rx_cur = 0;
static uint16_t tx_cur = 0;
static bool e1000_ready = false;
static spinlock_t e1000_lock = SPINLOCK_INIT;

static void mmio_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(e1000_mmio + reg) = val;
}

static uint32_t mmio_read32(uint32_t reg) {
    return *(volatile uint32_t *)(e1000_mmio + reg);
}

// Detect EEPROM and read MAC
static bool detect_eeprom(void) {
    mmio_write32(E1000_EEPROM, 0x1); 
    for (int i = 0; i < 1000 && !(mmio_read32(E1000_EEPROM) & 0x10); i++) io_wait();
    return (mmio_read32(E1000_EEPROM) & 0x10) != 0;
}

static uint16_t eeprom_read(uint8_t addr) {
    uint32_t temp = 0;
    mmio_write32(E1000_EEPROM, 1 | ((uint32_t)(addr) << 8));
    while (!((temp = mmio_read32(E1000_EEPROM)) & (1 << 4))) io_wait();
    return (uint16_t)((temp >> 16) & 0xFFFF);
}

void e1000_get_mac(uint8_t mac[6]) {
    memcpy(mac, mac_addr, 6);
}

bool e1000_send(const void *data, uint16_t len) {
    if (!e1000_ready) return false;

    uint64_t irq;
    spin_lock_irqsave(&e1000_lock, &irq);

    // Wait until current descriptor is available
    while (!(tx_descs[tx_cur].status & 0x01) && tx_descs[tx_cur].status != 0) {
        // Drop lock briefly to allow interrupts or other cores if needed
        spin_unlock_irqrestore(&e1000_lock, irq);
        io_wait();
        spin_lock_irqsave(&e1000_lock, &irq);
    }

    // Copy data into TX buffer
    uint16_t send_len = len;
    if (len < 60) send_len = 60; // Pad short packets
    memcpy(tx_buf[tx_cur], data, len);
    if (len < 60) memset(tx_buf[tx_cur] + len, 0, 60 - len);

    // Setup descriptor
    tx_descs[tx_cur].addr = (uint64_t)virt_to_phys(tx_buf[tx_cur]);
    tx_descs[tx_cur].length = send_len;
    tx_descs[tx_cur].cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    tx_descs[tx_cur].status = 0;

    // Advance tail pointer
    uint16_t old_cur = tx_cur;
    tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
    mmio_write32(E1000_TDT, tx_cur);

    spin_unlock_irqrestore(&e1000_lock, irq);

    // Block until sent (wait for RS flag to clear bit 0 status)
    while (!(tx_descs[old_cur].status & 0x01)) io_wait();

    return true;
}

static void e1000_poll(void) {
    if (!e1000_ready) return;

    // Read interrupt cause
    uint32_t icr = mmio_read32(E1000_ICR);
    if (!icr) return;

    if (icr & 0x80) { // Receiver Timer Interrupt (packet received)
        while ((rx_descs[rx_cur].status & 0x1)) {
            uint64_t irq;
            spin_lock_irqsave(&e1000_lock, &irq);

            // Re-read after locking just to be sure
            if (!(rx_descs[rx_cur].status & 0x1)) {
                spin_unlock_irqrestore(&e1000_lock, irq);
                break;
            }

            uint8_t *buf = rx_buf[rx_cur];
            uint16_t size = rx_descs[rx_cur].length;

            // Give descriptor back
            rx_descs[rx_cur].status = 0;

            uint16_t old_cur = rx_cur;
            rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;

            // Update tail to allow hardware to reuse
            mmio_write32(E1000_RDT, old_cur);

            spin_unlock_irqrestore(&e1000_lock, irq);

            // Pass packet to net_rx lock-free (net_lock handles its internal state)
            net_rx(buf, size);
        }
    }
}

void init_e1000(pci_device_t *dev) {
    if (!dev) return;

    // Enable bus mastering
    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x0004);

    // BAR0 should be MMIO
    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    e1000_mmio = (volatile uint8_t*)phys_to_virt(bar0 & ~0xF);

    // Read MAC Address
    if (detect_eeprom()) {
        uint16_t temp;
        temp = eeprom_read(0);
        mac_addr[0] = temp & 0xFF; mac_addr[1] = temp >> 8;
        temp = eeprom_read(1);
        mac_addr[2] = temp & 0xFF; mac_addr[3] = temp >> 8;
        temp = eeprom_read(2);
        mac_addr[4] = temp & 0xFF; mac_addr[5] = temp >> 8;
    } else {
        uint32_t mac_lo = mmio_read32(E1000_RAL);
        uint32_t mac_hi = mmio_read32(E1000_RAH);
        mac_addr[0] = mac_lo & 0xFF; mac_addr[1] = (mac_lo >> 8) & 0xFF;
        mac_addr[2] = (mac_lo >> 16) & 0xFF; mac_addr[3] = (mac_lo >> 24) & 0xFF;
        mac_addr[4] = mac_hi & 0xFF; mac_addr[5] = (mac_hi >> 8) & 0xFF;
    }

    // Setup Multicast Table
    for (int i = 0; i < 128; i++) mmio_write32(E1000_MTA + (i * 4), 0);

    // Setup RX ring (must be 16-byte aligned and contiguous physically)
    rx_descs = pmalloc(); // 4KB page, plenty of room for 32 descriptors
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_buf[i] = vmalloc(8192); // Packets max 1522 bytes, allocator returns whole pages
        rx_descs[i].addr = (uint64_t)virt_to_phys(rx_buf[i]);
        rx_descs[i].status = 0;
    }

    // Write RX rings
    uint64_t rx_phys = (uint64_t)virt_to_phys(rx_descs);
    mmio_write32(E1000_RDBAL, rx_phys & 0xFFFFFFFF);
    mmio_write32(E1000_RDBAH, rx_phys >> 32);
    mmio_write32(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    mmio_write32(E1000_RDH, 0);
    mmio_write32(E1000_RDT, E1000_NUM_RX_DESC - 1); // Last valid index

    // Enable RX
    mmio_write32(E1000_RCTL, RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE | RCTL_LPE | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    // Setup TX ring
    tx_descs = pmalloc(); 
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_buf[i] = vmalloc(8192);
        tx_descs[i].addr = (uint64_t)virt_to_phys(tx_buf[i]);
        tx_descs[i].cmd = 0;
        tx_descs[i].status = 1; // Mark as done/free initially
    }

    // Write TX rings
    uint64_t tx_phys = (uint64_t)virt_to_phys(tx_descs);
    mmio_write32(E1000_TDBAL, tx_phys & 0xFFFFFFFF);
    mmio_write32(E1000_TDBAH, tx_phys >> 32);
    mmio_write32(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    mmio_write32(E1000_TDH, 0);
    mmio_write32(E1000_TDT, 0);

    // Enable TX
    mmio_write32(E1000_TCTL, TCTL_EN | TCTL_PSP);

    // Enable interrupts
    mmio_write32(E1000_IMS, 0x1F6DC);
    mmio_write32(E1000_IMS, 0xFF & ~4);
    mmio_read32(E1000_ICR); // clear pending

    register_pci_interrupt_handler(e1000_poll);
    e1000_ready = true;
}
