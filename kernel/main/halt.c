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

__attribute__((noreturn)) void halt(void) {
    system_halted = 1;
    asm volatile("cli" : : : "memory");
    for (;;) asm volatile("hlt" : : : "memory");
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
