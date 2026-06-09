#pragma once

#define __cpuid(level, a, b, c, d) __asm__ volatile ("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "0" (level))
#define __cpuid_count(level, count, a, b, c, d) __asm__ volatile ("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "0" (level), "2" (count))

static inline int get_cpuid(unsigned int leaf, unsigned int subleaf, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d) {
    unsigned int max_leaf, ext_b, ext_c, ext_d;
#if defined(__i386__) || defined(__x86_64__)
    unsigned long long f1, f2;
    __asm__ volatile (
        "pushf\n\t"
        "pushf\n\t"
        "pop %0\n\t"
        "mov %0, %1\n\t"
        "xor $0x200000, %0\n\t"
        "push %0\n\t"
        "popf\n\t"
        "pushf\n\t"
        "pop %0\n\t"
        "popf"
        : "=&r" (f1), "=&r" (f2)
        :
        : "memory"
    );

    if (((f1 ^ f2) & 0x200000) == 0) return 0;
#endif
    __cpuid(0, max_leaf, ext_b, ext_c, ext_d);

    if ((leaf & 0x80000000) != 0) __cpuid(0x80000000, max_leaf, ext_b, ext_c, ext_d);

    if (max_leaf == 0 || max_leaf < leaf) return 0;

    __cpuid_count(leaf, subleaf, *a, *b, *c, *d);

    return 1;
}
