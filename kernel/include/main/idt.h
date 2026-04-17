#pragma once

#include <freestanding/stdint.h>

// The IDT entry structure
struct idt_entry {
    uint16_t isr_low;   // The lower 16 bits of the ISR's address
    uint16_t kernel_cs; // The GDT segment selector that the CPU will load before calling the ISR
    uint8_t ist;       // The IST in the TSS
    uint8_t attributes;// Type and attributes
    uint16_t isr_mid;   // The higher 16 bits of the lower 32 bits of the ISR's address
    uint32_t isr_high;  // The higher 32 bits of the ISR's address
    uint32_t reserved;  // Set to zero
} __attribute__((packed));

// The IDTR structure
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void load_idt_for_cpu(void);
void init_idt(void);