#include <stdbool.h>
#include <stdint.h>
#include <main/smap.h>

#define CPUID_STRUCTURED_EXTENDED_FEATURES 7U
#define CPUID_SMAP (1U << 20)
#define CR4_SMAP (1ULL << 21)

bool smap_enabled = false;

void enable_smap(void) {
    uint32_t maximum_leaf;
    uint32_t ebx;
    uint32_t unused_ecx;
    uint32_t unused_edx;
    __asm__ volatile("cpuid" : "=a"(maximum_leaf), "=b"(ebx), "=c"(unused_ecx), "=d"(unused_edx) : "a"(0));
    if (maximum_leaf < CPUID_STRUCTURED_EXTENDED_FEATURES) return;

    uint32_t unused_eax;
    __asm__ volatile("cpuid" : "=a"(unused_eax), "=b"(ebx), "=c"(unused_ecx), "=d"(unused_edx) : "a"(CPUID_STRUCTURED_EXTENDED_FEATURES), "c"(0));
    if (!(ebx & CPUID_SMAP)) return;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    __asm__ volatile("clac" ::: "memory");
    smap_enabled = true;
}
