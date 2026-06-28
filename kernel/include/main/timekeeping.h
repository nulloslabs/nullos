#pragma once

#include <freestanding/stdint.h>

uint64_t time_get_realtime_us(void);
void time_set_realtime_us(uint64_t usec);
void time_seed_realtime_us(uint64_t usec);

