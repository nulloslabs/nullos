#pragma once

#include <stdbool.h>

#define CR4_SMEP (1ULL << 20)

extern bool smep_enabled;

void enable_smep_for_cpu(void);
void enable_smep(void);
