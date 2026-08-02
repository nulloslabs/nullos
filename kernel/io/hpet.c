#include <stdint.h>
#include <io/hpet.h>
#include <io/terminal.h>
#include <mm/vmm.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

static uintptr_t hpet_base;
static uint32_t hpet_period;

void sleep(uint64_t ms) {
    if (!hpet_base || !hpet_period) return;
    volatile uint64_t *counter = (volatile uint64_t *)(hpet_base + 0xF0);
    uint64_t ticks = (ms * 1000000000000ULL) / hpet_period;
    uint64_t start = *counter;
    while (*counter - start < ticks) __asm__ volatile("pause");
}

void sleep_us(uint64_t us) {
    if (!hpet_base || !hpet_period) return;
    volatile uint64_t *counter = (volatile uint64_t *)(hpet_base + 0xF0);
    uint64_t ticks = (us * 1000000000ULL) / hpet_period;
    uint64_t start = *counter;
    while (*counter - start < ticks) __asm__ volatile("pause");
}

uint64_t read_hpet_counter(void) {
    if (!hpet_base || !hpet_period) return 0;
    return *(volatile uint64_t *)(hpet_base + 0xF0);
}

uint64_t hpet_elapsed_us(void) {
    if (!hpet_base || !hpet_period) return 0;
    return (read_hpet_counter() * hpet_period) / 1000000000ULL;
}

uint32_t get_hpet_freq_mhz(void) {
    if (!hpet_base || !hpet_period) return 0;
    return (uint32_t)(1000000000000000ULL / hpet_period / 1000000);
}

void stop_hpet(void) {
    if (!hpet_base || !hpet_period) return;
    volatile uint64_t *config = (volatile uint64_t *)(hpet_base + 0x10);
    *config &= ~1ULL;
    __asm__ volatile("" ::: "memory");
}

void init_hpet(void) {
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &table) != UACPI_STATUS_OK) return;
    struct acpi_hpet *hpet = table.ptr;
    if (hpet->address.address_space_id != 0 || !hpet->address.address) {
        uacpi_table_unref(&table);
        return;
    }
    hpet_base = (uintptr_t)vmap_mmio(hpet->address.address, 1);
    uacpi_table_unref(&table);
    if (!hpet_base) return;
    volatile uint64_t *capabilities = (volatile uint64_t *)hpet_base;
    volatile uint64_t *config = (volatile uint64_t *)(hpet_base + 0x10);
    hpet_period = (uint32_t)(*capabilities >> 32);
    if (!hpet_period) {
        hpet_base = 0;
        return;
    }
    *config |= 1;
    printf("hpet: initialized hpet\n");
}
