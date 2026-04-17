#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

// APIC modes detected via CPUID
enum apic_mode {
    APIC_NONE = 0, // No APIC, use 8259 PIC
    APIC_XAPIC, // xAPIC (MMIO-based)
    APIC_X2APIC // x2APIC (MSR-based)
};

extern enum apic_mode current_apic_mode;
extern volatile uint32_t *lapic_base;

// Detection and initialization
enum apic_mode detect_apic(void);
void init_apic(void);

// Common API (dispatches based on current_apic_mode)
void eoi_apic(void);
uint32_t get_apic_id(void);
void init_apic_timer(uint32_t frequency_hz);
void send_apic_ipi(uint32_t apic_id, uint32_t vector);

// LAPIC register offsets (for xAPIC MMIO)
#define LAPIC_ID 0x020
#define LAPIC_VERSION 0x030
#define LAPIC_TPR 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_SVR 0x0F0
#define LAPIC_ICR_LO 0x300
#define LAPIC_ICR_HI 0x310
#define LAPIC_TIMER_LVT 0x320
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_CCR 0x390
#define LAPIC_TIMER_DCR 0x3E0

// x2APIC MSR base
#define X2APIC_MSR_BASE  0x800
#define X2APIC_MSR_ID    0x802
#define X2APIC_MSR_EOI   0x80B
#define X2APIC_MSR_SVR   0x80F
#define X2APIC_MSR_ICR   0x830
#define X2APIC_MSR_LVT_TIMER 0x832
#define X2APIC_MSR_TIMER_ICR 0x838
#define X2APIC_MSR_TIMER_CCR 0x839
#define X2APIC_MSR_TIMER_DCR 0x83E

// SVR bits
#define LAPIC_SVR_ENABLE 0x100

// Timer modes
#define LAPIC_TIMER_PERIODIC (1 << 17)
#define LAPIC_TIMER_MASKED   (1 << 16)

// IA32_APIC_BASE MSR
#define MSR_APIC_BASE       0x1B
#define MSR_APIC_BASE_EN    (1 << 11)
#define MSR_APIC_BASE_X2EN  (1 << 10)
