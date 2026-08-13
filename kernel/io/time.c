#include <main/panic.h>
#include <io/hpet.h>
#include <io/time.h>
#include <io/tsc.h>

static bool hpet_epoch_initialized = false;
static int64_t hpet_epoch_offset_us = 0;

uint64_t get_monotonic_time_us(void) {
    if (is_hpet_available()) {
        uint64_t hpet_us = get_elapsed_hpet_us();
        if (!hpet_epoch_initialized) {
            uint64_t reference_us = is_tsc_available() ? get_elapsed_tsc_us() : hpet_us;
            hpet_epoch_offset_us = reference_us >= hpet_us
                ? (int64_t)(reference_us - hpet_us)
                : -(int64_t)(hpet_us - reference_us);
            hpet_epoch_initialized = true;
        }
        if (hpet_epoch_offset_us >= 0) return hpet_us + (uint64_t)hpet_epoch_offset_us;
        uint64_t offset = (uint64_t)-hpet_epoch_offset_us;
        return hpet_us >= offset ? hpet_us - offset : 0;
    }
    if (is_tsc_available()) return get_elapsed_tsc_us();
    return 0;
}

uint64_t get_raw_time_counter(void) {
    if (is_tsc_available()) return read_tsc();
    if (is_hpet_available()) return read_hpet_counter();
    return 0;
}

void sleep(uint64_t ms) {
    if (is_hpet_available()) { sleep_hpet(ms); return; }
    if (is_tsc_available()) { sleep_tsc(ms); return; }
    panic("no timer available for sleep");
}

void sleep_us(uint64_t us) {
    if (is_hpet_available()) { sleep_hpet_us(us); return; }
    if (is_tsc_available()) { sleep_tsc_us(us); return; }
    panic("no timer available for sleep");
}
