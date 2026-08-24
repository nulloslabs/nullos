#include <main/log.h>
#include <io/ioapic.h>
#include <mm/vmm.h>

volatile uint32_t *ioapic_base = NULL;

static uint32_t read_ioapic(uint32_t reg) {
    ioapic_base[IOAPIC_REGSEL / 4] = reg;
    return ioapic_base[IOAPIC_WIN / 4];
}
static void write_ioapic(uint32_t reg, uint32_t val) {
     ioapic_base[IOAPIC_REGSEL / 4] = reg;
     ioapic_base[IOAPIC_WIN / 4] = val;
}

uint32_t max_ioapic_redirects(void) {
    return ((read_ioapic(IOAPIC_VER) >> 16) & 0xFF) + 1;
}

void route_ioapic_irq(uint8_t irq, uint8_t vector, uint32_t lapic_id, uint32_t flags) {
    uint32_t reg_lo = IOAPIC_REDTBL + (irq * 2);
    uint32_t reg_hi = IOAPIC_REDTBL + (irq * 2) + 1;

    // High: destination APIC ID (bits 24-31 for physical dest)
    write_ioapic(reg_hi, (lapic_id & 0xFF) << 24);

    // Low: vector + flags (polarity, trigger, mask, delivery mode)
    write_ioapic(reg_lo, vector | flags);
}

void init_ioapic(void *base_addr) {
    // Map IOAPIC MMIO (typically at 0xFEC00000)
    ioapic_base = (volatile uint32_t *)base_addr;

    uint32_t ver = read_ioapic(IOAPIC_VER);
    uint32_t max_irqs = ((ver >> 16) & 0xFF) + 1;

    // Mask all IRQs first
    for (uint32_t i = 0; i < max_irqs; i++) {
        uint32_t reg_lo = IOAPIC_REDTBL + (i * 2);
        write_ioapic(reg_lo, IOAPIC_INT_MASKED);
    }

    log("ioapic: initialized ioapic\n");
}
