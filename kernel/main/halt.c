#include <main/halt.h>
#include <io/apic.h>
#include <main/mp.h>

volatile int system_halted = 0;

void cli(void) {
    asm volatile("cli" : : : "memory");
}

void sti(void) {
    asm volatile("sti" : : : "memory");
}

static void halt_other_cpus(void) {
    if (current_apic_mode == APIC_NONE || cpu_count <= 1)
        return;

    uint32_t self = get_apic_id();
    for (int i = 0; i < cpu_count; i++) {
        if (!cpus[i].active || cpus[i].lapic_id == self)
            continue;
        send_init_apic(cpus[i].lapic_id);
    }
}

__attribute__((noreturn)) void halt(void) {
    if (!__sync_lock_test_and_set(&system_halted, 1))
        halt_other_cpus();

    asm volatile("cli" : : : "memory");
    for (;;) asm volatile("hlt" : : : "memory");
}

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
