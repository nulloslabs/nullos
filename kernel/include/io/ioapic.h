#pragma once

#include <freestanding/stdint.h>

// IOAPIC register offsets (indirect via IOREGSEL/IOWIN)
#define IOAPIC_REGSEL   0x00
#define IOAPIC_WIN      0x10

// IOAPIC registers
#define IOAPIC_ID       0x00
#define IOAPIC_VER      0x01
#define IOAPIC_ARB      0x02
#define IOAPIC_REDTBL   0x10  // Base of redirection table (entry N = 0x10 + 2*N)

// Redirection entry flags
#define IOAPIC_INT_MASKED   (1 << 16)
#define IOAPIC_TRIGGER_LEVEL (1 << 15)
#define IOAPIC_ACTIVE_LOW   (1 << 13)

extern volatile uint32_t *ioapic_base;

void init_ioapic(void *base_addr);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t lapic_id, uint32_t flags);
uint32_t ioapic_max_redirects(void);
