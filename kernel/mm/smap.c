#include <stdint.h>
#include <stdbool.h>
#include <main/machine_info.h>
#include <main/log.h>
#include <mm/smap.h>
#include <mm/smep.h>

bool smap_enabled = false;

void stac(void) {
    if (smap_enabled) __asm__ volatile("stac" ::: "memory");
}

void clac(void) {
    if (smap_enabled) __asm__ volatile("clac" ::: "memory");
}

void enable_smap_for_cpu(void) {
    if (!cpu_has_feature(CPU_FEATURE_SMAP)) return;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    smap_enabled = true;
    clac();
}

void enable_smap(void) {
    enable_smap_for_cpu();
    if (smap_enabled) log("smap: enabled smap\n");
}
