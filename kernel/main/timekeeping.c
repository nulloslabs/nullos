#include <freestanding/stdint.h>
#include <io/hpet.h>
#include <main/spinlocks.h>
#include <main/timekeeping.h>

static spinlock_t time_lock = SPINLOCK_INIT;
static int64_t wall_time_offset_us = 0;

uint64_t time_get_realtime_us(void) {
    uint64_t irq;
    uint64_t uptime_us = hpet_elapsed_us();
    int64_t offset_us;

    spin_lock_irqsave(&time_lock, &irq);
    offset_us = wall_time_offset_us;
    spin_unlock_irqrestore(&time_lock, irq);

    int64_t realtime_us = (int64_t)uptime_us + offset_us;
    if (realtime_us < 0) {
        return 0;
    }

    return (uint64_t)realtime_us;
}

void time_set_realtime_us(uint64_t usec) {
    uint64_t irq;
    int64_t uptime_us = (int64_t)hpet_elapsed_us();

    spin_lock_irqsave(&time_lock, &irq);
    wall_time_offset_us = (int64_t)usec - uptime_us;
    spin_unlock_irqrestore(&time_lock, irq);
}

void time_seed_realtime_us(uint64_t usec) {
    time_set_realtime_us(usec);
}
