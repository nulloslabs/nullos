#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>
#include <main/string.h>
#include <main/machine_info.h>
#include <io/hpet.h>
#include <io/terminal.h>
#include <main/limine_req.h>

static uint64_t round_up_ram(uint64_t x) {
    if (x >= (1024ULL * 1024 * 1024)) {
        return (x + (1024ULL * 1024 * 1024) - 1) & ~((1024ULL * 1024 * 1024) - 1);
    } else {
        return (x + (1024ULL * 1024) - 1) & ~((1024ULL * 1024) - 1);
    }
}

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

const char* get_cpu_name(void) {
    static char buf[49] = {0};
    if (buf[0]) return buf;
    uint32_t eax, ebx, ecx, edx;
    for (int i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, 0, &eax, &ebx, &ecx, &edx);
        memcpy(buf + i * 16 + 0,  &eax, 4);
        memcpy(buf + i * 16 + 4,  &ebx, 4);
        memcpy(buf + i * 16 + 8,  &ecx, 4);
        memcpy(buf + i * 16 + 12, &edx, 4);
    }
    buf[48] = '\0';
    return buf;
}

const char* get_cpu_vendor(void) {
    static char buf[13] = {0};
    if (buf[0]) return buf;
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    memcpy(buf + 0, &ebx, 4);
    memcpy(buf + 4, &edx, 4);
    memcpy(buf + 8, &ecx, 4);
    buf[12] = '\0';
    return buf;
}

uint32_t get_cpu_family(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint32_t family = (eax >> 8) & 0xF;
    if (family == 0xF)
        family += (eax >> 20) & 0xFF;
    cached = family;
    return cached;
}

uint32_t get_cpu_model(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint32_t model  = (eax >> 4) & 0xF;
    uint32_t family = (eax >> 8) & 0xF;
    if (family == 0x6 || family == 0xF)
        model |= ((eax >> 16) & 0xF) << 4;
    cached = model;
    return cached;
}

uint32_t get_cpu_stepping(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    cached = eax & 0xF;
    return cached;
}

uint32_t get_cpu_cores(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint32_t eax, ebx, ecx, edx;
    cpuid(0xB, 1, &eax, &ebx, &ecx, &edx);
    if (ebx != 0) { cached = ebx & 0xFFFF; return cached; }
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    cached = (ebx >> 16) & 0xFF;
    return cached;
}

uint32_t get_cpu_threads(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint32_t eax, ebx, ecx, edx;
    cpuid(0xB, 0, &eax, &ebx, &ecx, &edx);
    if (ebx != 0) { cached = ebx & 0xFFFF; return cached; }
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    cached = (ebx >> 16) & 0xFF;
    return cached;
}

uint32_t get_cpu_freq(void) {
    static uint32_t cached = 0.0f;
    if (cached != 0) return cached;
    uint32_t freq_mhz = get_hpet_freq_mhz();
    if (!freq_mhz) return 0.0f;
    uint32_t samples[5];
    for (int i = 0; i < 5; i++) {
        uint64_t start = read_hpet_counter();
        uint64_t tsc_start;
        asm volatile("rdtsc" : "=A"(tsc_start));
        uint64_t ticks = (uint64_t)freq_mhz * 70000;
        while (read_hpet_counter() - start < ticks);
        uint64_t tsc_end;
        asm volatile("rdtsc" : "=A"(tsc_end));
        samples[i] = (uint32_t)((tsc_end - tsc_start) / 70000);
    }
    uint64_t sum = 0;
    for (int i = 0; i < 5; i++) sum += samples[i];
    cached = (uint32_t)(sum / 5);
    return cached;
}

bool cpu_has_feature(cpu_feature_t feature) {
    static uint32_t ecx1 = 0, edx1 = 0, ebx7 = 0;
    static bool initialized = false;
    if (!initialized) {
        uint32_t eax, ebx, ecx, edx;
        cpuid(1, 0, &eax, &ebx, &ecx1, &edx1);
        cpuid(7, 0, &eax, &ebx7, &ecx, &edx);
        initialized = true;
    }
    switch (feature) {
        case CPU_FEATURE_SSE:    return (edx1 >> 25) & 1;
        case CPU_FEATURE_SSE2:   return (edx1 >> 26) & 1;
        case CPU_FEATURE_SSE3:   return (ecx1 >> 0) & 1;
        case CPU_FEATURE_SSSE3:  return (ecx1 >> 9) & 1;
        case CPU_FEATURE_SSE41:  return (ecx1 >> 19) & 1;
        case CPU_FEATURE_SSE42:  return (ecx1 >> 20) & 1;
        case CPU_FEATURE_AVX:    return (ecx1 >> 28) & 1;
        case CPU_FEATURE_FPU:    return (edx1 >> 0) & 1;
        case CPU_FEATURE_XAPIC:   return (edx1 >> 9) & 1;
        case CPU_FEATURE_X2APIC: return (ecx1 >> 21) & 1;
        case CPU_FEATURE_POPCNT: return (ecx1 >> 23) & 1;
        case CPU_FEATURE_AES:    return (ecx1 >> 25) & 1;
        case CPU_FEATURE_AVX2:   return (ebx7 >> 5) & 1;
        default: return false;
    }
}

uint64_t get_total_ram(void) {
    static uint64_t cached = 0;
    if (cached) return cached;
    struct limine_memmap_response *mm_map = mm_req.response;
    for (uint64_t i = 0; i < mm_map->entry_count; i++) {
        struct limine_memmap_entry *entry = mm_map->entries[i];
        if (entry->type != LIMINE_MEMMAP_FRAMEBUFFER && entry->type != LIMINE_MEMMAP_BAD_MEMORY) cached += entry->length;
    }
    return round_up_ram(cached); // Round up to the power of 2
}

uint64_t get_free_ram(void) {
    static uint64_t cached = 0;
    if (cached) return cached;
    struct limine_memmap_response *mm_map = mm_req.response;
    for (uint64_t i = 0; i < mm_map->entry_count; i++) {
        struct limine_memmap_entry *entry = mm_map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) cached += entry->length;
    }
    return cached;
}

uint64_t get_used_ram(void) {
    static uint64_t cached = 0;
    if (cached) return cached;
    cached = get_total_ram() - get_free_ram();
    return cached;
}

void cache_machine_info(void) {
    // Cache any info that hasn't already been cached.
    get_cpu_name();
    get_cpu_vendor();
    get_cpu_family();
    get_cpu_model();
    get_cpu_stepping();
    get_cpu_cores();
    get_cpu_threads();
    get_cpu_freq();
    cpu_has_feature(CPU_FEATURE_FPU); // One function already caches everything.
    get_total_ram();
    get_free_ram();
    get_used_ram();
    printf("Machine Info: Cached machine info.\n");
}
