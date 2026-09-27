#pragma once

#include <freestanding/stdint.h>

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_TSC_AUX 0xC0000103

#define MSR_EFER_SCE (1ULL << 0)
#define MSR_EFER_LME (1ULL << 8)
#define MSR_EFER_LMA (1ULL << 10)
#define MSR_EFER_NXE (1ULL << 11)
#define MSR_EFER_SVME (1ULL << 12)
#define MSR_EFER_LMSLE (1ULL << 13)
#define MSR_EFER_FFXSR (1ULL << 14)
#define MSR_EFER_TCE (1ULL << 15)

#define MSR_APIC_BASE       0x1B
#define MSR_APIC_BASE_EN    (1 << 11)
#define MSR_APIC_BASE_X2EN  (1 << 10)

uint64_t read_msr(uint32_t msr);
void write_msr(uint32_t msr, uint64_t val);