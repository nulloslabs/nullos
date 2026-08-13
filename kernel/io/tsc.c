#include <stdint.h>
#include <main/limine_req.h>
#include <io/tsc.h>

static void sleep_tsc_units(uint64_t duration, uint64_t units_per_second) {
    if (!is_tsc_available() || !duration) return;
    uint64_t frequency = tsc_req.response->frequency;
    uint64_t whole = duration / units_per_second;
    uint64_t remainder = duration % units_per_second;
    uint64_t ticks = whole > UINT64_MAX / frequency ? UINT64_MAX : whole * frequency;
    uint64_t fraction = (remainder * frequency) / units_per_second;
    if (UINT64_MAX - ticks < fraction) ticks = UINT64_MAX;
    else ticks += fraction;
    uint64_t start = read_tsc();
    while (read_tsc() - start < ticks) __asm__ volatile ("pause");
}

bool is_tsc_available(void) { return tsc_req.response && tsc_req.response->frequency; }

uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t get_elapsed_tsc_us(void) {
    if (!is_tsc_available()) return 0;
    uint64_t frequency = tsc_req.response->frequency;
    uint64_t ticks = read_tsc();
    return (ticks / frequency) * 1000000ULL + ((ticks % frequency) * 1000000ULL) / frequency;
}

void sleep_tsc(uint64_t ms) { sleep_tsc_units(ms, 1000); }
void sleep_tsc_us(uint64_t us) { sleep_tsc_units(us, 1000000); }
