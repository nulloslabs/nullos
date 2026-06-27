#include <io/apic.h>
#include <io/pic.h>
#include <io/io.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <io/terminal.h>
#include <main/machine_info.h>
#include <io/ioapic.h>
#include <main/madt.h>
#include <main/msr.h>

enum apic_mode current_apic_mode = APIC_NONE;
volatile uint32_t *lapic_base = NULL;

// xAPIC MMIO helpers
static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

// --- Detection ---
enum apic_mode detect_apic(void) {
    bool has_xapic = cpu_has_feature(CPU_FEATURE_XAPIC);
    bool has_x2apic = cpu_has_feature(CPU_FEATURE_X2APIC);
    if (has_xapic && has_x2apic) current_apic_mode = APIC_X2APIC;
    else if (has_xapic) current_apic_mode = APIC_XAPIC;
    else current_apic_mode = APIC_NONE;
    return current_apic_mode;
}

// --- Initialization ---
static void init_xapic_for_cpu(uint64_t base_phys) {
    // Map LAPIC MMIO into kernel address space
    lapic_base = (volatile uint32_t *)phys_to_virt(base_phys);

    // Enable LAPIC via IA32_APIC_BASE MSR
    uint64_t msr = read_msr(MSR_APIC_BASE);
    msr |= MSR_APIC_BASE_EN;
    msr &= ~MSR_APIC_BASE_X2EN; // Make sure x2APIC bit is clear
    write_msr(MSR_APIC_BASE, msr);

    // Set Spurious Interrupt Vector Register: enable + vector 0xFF
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

    // Clear TPR (accept all interrupts)
    lapic_write(LAPIC_TPR, 0);
}

static void init_x2apic_for_cpu(void) {
    // Enable x2APIC via IA32_APIC_BASE MSR
    uint64_t msr = read_msr(MSR_APIC_BASE);
    msr |= MSR_APIC_BASE_EN | MSR_APIC_BASE_X2EN;
    write_msr(MSR_APIC_BASE, msr);

    // Set SVR: enable + vector 0xFF
    write_msr(X2APIC_MSR_SVR, LAPIC_SVR_ENABLE | 0xFF);
}

void init_apic_for_cpu(void) {
    if (current_apic_mode == APIC_X2APIC) {
        init_x2apic_for_cpu();
    } else if (current_apic_mode == APIC_XAPIC) {
        uint64_t msr = read_msr(MSR_APIC_BASE);
        uint64_t base_phys = msr & 0xFFFFF000ULL;
        init_xapic_for_cpu(base_phys);
    }
}

void init_apic(void) {
    init_apic_for_cpu();
    if (current_apic_mode == APIC_X2APIC) {
        printf("apic: initialized x2apic\n");
    } else if (current_apic_mode == APIC_XAPIC) {
        printf("apic: initialized xapic\n");
    } else {
        printf("apic: no apic found, falling back to pic\n");
        return;
    }

    disable_pic();

    if (ioapic_phys_addr) {
        init_ioapic(phys_to_virt((uint64_t)ioapic_phys_addr));
        ioapic_route_irq(1, 33, 0, 0);
        for (int i = 0; i < 4; i++) {
            ioapic_route_irq(10 + i, 43, 0, IOAPIC_ACTIVE_LOW | IOAPIC_TRIGGER_LEVEL);
        }
    }
}

// --- EOI ---
void eoi_apic(void) {
    if (current_apic_mode == APIC_X2APIC) {
        write_msr(X2APIC_MSR_EOI, 0);
    } else if (current_apic_mode == APIC_XAPIC) {
        lapic_write(LAPIC_EOI, 0);
    } else {
        eoi_pic();
    }
}

// --- APIC ID ---
uint32_t get_apic_id(void) {
    if (current_apic_mode == APIC_X2APIC) {
        return (uint32_t)read_msr(X2APIC_MSR_ID);
    } else if (current_apic_mode == APIC_XAPIC) {
        return lapic_read(LAPIC_ID) >> 24;
    }
    return 0;
}

// --- LAPIC Timer ---
void init_apic_timer(uint32_t frequency_hz) {
    // Use divide-by-16
    uint32_t divide = 0x3; // divide by 16

    if (current_apic_mode == APIC_X2APIC) {
        write_msr(X2APIC_MSR_TIMER_DCR, divide);

        // Calibrate: use a short busy-wait with PIT channel 2
        // Set up PIT channel 2 for ~10ms one-shot
        outb(0x61, (inb(0x61) & 0xFD) | 0x01);
        outb(0x43, 0xB0); // Channel 2, lobyte/hibyte, mode 0
        uint16_t pit_count = 11932; // ~10ms at 1.193182 MHz
        outb(0x42, pit_count & 0xFF);
        outb(0x42, (pit_count >> 8) & 0xFF);

        // Reset PIT gate to start counting
        uint8_t tmp = inb(0x61) & 0xFE;
        outb(0x61, tmp);
        outb(0x61, tmp | 1);

        // Start LAPIC timer with max initial count
        write_msr(X2APIC_MSR_LVT_TIMER, LAPIC_TIMER_MASKED | 32);
        write_msr(X2APIC_MSR_TIMER_ICR, 0xFFFFFFFF);

        // Wait for PIT to expire
        while (!(inb(0x61) & 0x20));

        // Read how many ticks elapsed
        uint32_t elapsed = 0xFFFFFFFF - (uint32_t)read_msr(X2APIC_MSR_TIMER_CCR);
        // Scale to desired frequency
        uint32_t ticks_per_interval = (elapsed * 100) / (1000 / (1000 / frequency_hz));
        if (ticks_per_interval == 0) ticks_per_interval = elapsed * 100 / 10; // fallback for 100 Hz

        // Set periodic mode on vector 32
        write_msr(X2APIC_MSR_LVT_TIMER, LAPIC_TIMER_PERIODIC | 32);
        write_msr(X2APIC_MSR_TIMER_ICR, ticks_per_interval);
    } else if (current_apic_mode == APIC_XAPIC) {
        lapic_write(LAPIC_TIMER_DCR, divide);

        // Calibrate with PIT channel 2
        outb(0x61, (inb(0x61) & 0xFD) | 0x01);
        outb(0x43, 0xB0);
        uint16_t pit_count = 11932;
        outb(0x42, pit_count & 0xFF);
        outb(0x42, (pit_count >> 8) & 0xFF);

        uint8_t tmp = inb(0x61) & 0xFE;
        outb(0x61, tmp);
        outb(0x61, tmp | 1);

        lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED | 32);
        lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

        while (!(inb(0x61) & 0x20));

        uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
        uint32_t ticks_per_interval = (elapsed * 100) / (1000 / (1000 / frequency_hz));
        if (ticks_per_interval == 0) ticks_per_interval = elapsed * 100 / 10;

        lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_PERIODIC | 32);
        lapic_write(LAPIC_TIMER_ICR, ticks_per_interval);
    }
}

// --- IPI ---
void send_apic_ipi(uint32_t target_apic_id, uint32_t vector) {
    if (current_apic_mode == APIC_X2APIC) {
        // x2APIC: single 64-bit write to ICR MSR
        uint64_t icr = ((uint64_t)target_apic_id << 32) | vector;
        write_msr(X2APIC_MSR_ICR, icr);
    } else if (current_apic_mode == APIC_XAPIC) {
        // xAPIC: write destination to ICR_HI, then command to ICR_LO
        lapic_write(LAPIC_ICR_HI, target_apic_id << 24);
        lapic_write(LAPIC_ICR_LO, vector);
        // Wait for delivery
        while (lapic_read(LAPIC_ICR_LO) & (1 << 12));
    }
}

void send_init_apic(uint32_t target_apic_id) {
    uint32_t command = LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT;

    if (current_apic_mode == APIC_X2APIC) {
        uint64_t icr = ((uint64_t)target_apic_id << 32) | command;
        write_msr(X2APIC_MSR_ICR, icr);
    } else if (current_apic_mode == APIC_XAPIC) {
        lapic_write(LAPIC_ICR_HI, target_apic_id << 24);
        lapic_write(LAPIC_ICR_LO, command);
        while (lapic_read(LAPIC_ICR_LO) & (1 << 12));
    }
}
