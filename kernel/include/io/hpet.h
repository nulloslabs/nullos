#pragma once

#include <stdint.h>
#include <stdbool.h>

bool is_hpet_available(void);
uint64_t read_hpet_counter(void);
uint64_t get_elapsed_hpet_us(void);
uint32_t get_hpet_freq_mhz(void);
void sleep_hpet(uint64_t ms);
void sleep_hpet_us(uint64_t us);
void stop_hpet(void);
void init_hpet(void);
