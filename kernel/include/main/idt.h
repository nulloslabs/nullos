#pragma once

#include <stdint.h>

struct idt_entry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t ist;
    uint8_t attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void load_idt_for_cpu(void);
void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags);
extern void acpi_isr(void);
void init_idt(void);
