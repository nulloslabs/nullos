#include <stdint.h>
#include <stdbool.h>
#include <main/machine_info.h>
#include <main/log.h>
#include <mm/smep.h>

bool smep_enabled = false;

void enable_smep_for_cpu(void) {
    if (!cpu_has_feature(CPU_FEATURE_SMEP)) return;

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_SMEP;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void enable_smep(void) {
    if (!cpu_has_feature(CPU_FEATURE_SMEP)) return;
    enable_smep_for_cpu();
    smep_enabled = true;
    log("smep: enabled smep\n");
}
