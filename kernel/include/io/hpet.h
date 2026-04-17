#pragma once

#include <freestanding/stdint.h>

void init_hpet(void);
void sleep(uint64_t ms);
void sleep_us(uint64_t us);
uint64_t read_hpet_counter(void);
uint32_t get_hpet_freq_mhz(void);
void stop_hpet(void);