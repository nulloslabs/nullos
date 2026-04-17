#pragma once

#include <freestanding/stdint.h>

struct madt_header {
    uint32_t local_apic_addr;
    uint32_t flags;
} __attribute__((packed));

struct madt_record {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

// Optional MADT parser if you want IOAPIC routing
void parse_madt(void);
extern void* ioapic_phys_addr;
