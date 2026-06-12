#pragma once

#include <freestanding/stdint.h>

__attribute__((noreturn)) void panic(const char *reason);
__attribute__((noreturn)) void exception_panic(uint64_t vector, uint64_t rip, uint64_t rsp, uint64_t cs);