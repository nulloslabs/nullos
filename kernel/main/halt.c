#include <main/halt.h>
#include <io/apic.h>
#include <io/pic.h>
#include <io/io.h>

volatile int system_halted = 0;

// MSR helpers (defined in apic.c, need local copies to avoid circular deps)
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    asm volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

// Disable interrupts
void cli(void) {
    asm volatile("cli" : : : "memory");
}

// Enable interrupts
void sti(void) {
    asm volatile("sti" : : : "memory");
}

// Full system halt - disable all interrupt sources before halting
__attribute__((noreturn)) void halt(void) {
    // 0. Signal all CPUs to stop
    system_halted = 1;

    // 1. Disable local interrupts
    asm volatile("cli" : : : "memory");
    
    // 2. Disable APIC timer (mask LVT timer)
    if (current_apic_mode == APIC_X2APIC) {
        wrmsr(0x832, rdmsr(0x832) | (1u << 16));  // Mask LVT timer
    } else if (current_apic_mode == APIC_XAPIC && lapic_base) {
        lapic_base[0x320 / 4] |= (1u << 16);  // Mask LVT timer
    }
    
    // 3. Send final EOI to clear any pending interrupts
    if (current_apic_mode == APIC_X2APIC) {
        wrmsr(0x80B, 0);
    } else if (current_apic_mode == APIC_XAPIC && lapic_base) {
        lapic_base[0xB0 / 4] = 0;
    } else {
        eoi_pic();
    }
    
    disable_pic();
    
    for (;;) {
        asm volatile("hlt" : : : "memory");
    }
}

// Idle halt
__attribute__((noreturn)) void idle(void) {
    for (;;) asm volatile("hlt" : : : "memory");
}

void pause(void) {
    asm volatile("pause" : : : "memory");
}

// Wait for interrupt
void wfi(void) {
    asm volatile("sti; pause; hlt" : : : "memory");
}