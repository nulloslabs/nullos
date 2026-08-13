#pragma once

#include <stdint.h>

uint64_t get_monotonic_time_us(void);
uint64_t get_raw_time_counter(void);
void sleep(uint64_t ms);
void sleep_us(uint64_t us);
