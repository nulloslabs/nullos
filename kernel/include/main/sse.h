#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

size_t get_fpu_state_size(void);
void save_fpu_state(void *area);
void restore_fpu_state(const void *area);
void init_fpu_area(void *area);
void init_sse(void);
