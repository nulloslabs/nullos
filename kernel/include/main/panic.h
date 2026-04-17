#pragma once

#include <freestanding/stdint.h>

void panic(const char *reason);
void exception_panic(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t rsp, uint64_t cs);