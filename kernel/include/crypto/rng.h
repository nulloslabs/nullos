#pragma once

#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>

void add_entropy_bytes(const void *buf, size_t len);
void get_random_bytes(void *buf, size_t len);
void init_rng(void);
bool is_rng_seeded(void);
