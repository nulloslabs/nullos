#pragma once

#include <stdint.h>
#include <stdbool.h>

bool is_tsc_available(void);
uint64_t read_tsc(void);
uint64_t get_elapsed_tsc_us(void);
void sleep_tsc(uint64_t ms);
void sleep_tsc_us(uint64_t us);
