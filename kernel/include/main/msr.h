#pragma once

#include <freestanding/stdint.h>

#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084

uint64_t read_msr(uint32_t msr);
void write_msr(uint32_t msr, uint64_t val);
