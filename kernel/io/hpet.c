#include <freestanding/stdint.h>
#include <main/acpi.h>
#include <io/hpet.h>
#include <io/terminal.h>
#include <main/panic.h>
#include <io/pit.h>
#include <mm/mm.h>

static uintptr_t hpet_base = 0;
static uint32_t hpet_period = 0;

void sleep(uint64_t ms) {
    if (!hpet_base || hpet_period == 0) {
        panic("HPET isn't available.");
    }
    volatile uint64_t* hpet_main_counter = (volatile uint64_t*)(hpet_base + 0xF0);
    uint64_t ticks_to_wait = (ms * 1000000000000ULL) / hpet_period;
    uint64_t start_tick = *hpet_main_counter;
    while ((*hpet_main_counter - start_tick) < ticks_to_wait) {
        asm volatile("pause");
    }
}

void sleep_us(uint64_t us) {
    if (!hpet_base || hpet_period == 0) {
        panic("HPET isn't available.");
    }
    volatile uint64_t* hpet_main_counter = (volatile uint64_t*)(hpet_base + 0xF0);
    uint64_t ticks_to_wait = (us * 1000000000ULL) / hpet_period;
    uint64_t start_tick = *hpet_main_counter;
    while ((*hpet_main_counter - start_tick) < ticks_to_wait) {
        asm volatile("pause");
    }
}

void init_hpet(void) {
    uint64_t phys = 0xFED00000ULL;
    struct acpi_header* hpet_tbl = find_acpi_table("HPET");
    if (hpet_tbl) {
        uint64_t tbl_phys = *(uint64_t*)((uint8_t*)hpet_tbl + 0x2C);
    }
    hpet_base = (uintptr_t)(phys + hhdm_offset);
    volatile uint64_t* hpet_capabilities = (volatile uint64_t*)hpet_base;
    volatile uint64_t* hpet_config = (volatile uint64_t*)(hpet_base + 0x10);
    hpet_period = (uint32_t)(*hpet_capabilities >> 32);
    if (hpet_period == 0 || hpet_period > 100000000) { hpet_base = 0; return; }
    *hpet_config |= 1;
    printf("HPET: Initialized HPET.\n");
}

uint64_t read_hpet_counter(void) {
    if (!hpet_base || hpet_period == 0) return 0;
    volatile uint64_t* hpet_main_counter = (volatile uint64_t*)(hpet_base + 0xF0);
    return *hpet_main_counter;
}

uint32_t get_hpet_freq_mhz(void) {
    if (!hpet_base || hpet_period == 0) return 0;
    // hpet_period is in femtoseconds, convert to MHz
    return (uint32_t)(1000000000000000ULL / hpet_period / 1000000);
}

void stop_hpet(void) {
    // Return if hpet_base is 0 (why did you even call this function if you didn't even init the HPET xD)
    if (!hpet_base || hpet_period == 0) return;

    volatile uint64_t* hpet_config = (volatile uint64_t*)(hpet_base + 0x10);

    if (*hpet_config & 1) {
        *hpet_config &= ~1ULL;
        // Compiler barrier (juuuust in case)
        asm volatile ("" : : : "memory");
    }
}
