#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

typedef enum {
    CPU_FEATURE_FPU,
    CPU_FEATURE_SSE,
    CPU_FEATURE_SSE2,
    CPU_FEATURE_SSE3,
    CPU_FEATURE_SSSE3,
    CPU_FEATURE_SSE41,
    CPU_FEATURE_SSE42,
    CPU_FEATURE_AVX,
    CPU_FEATURE_AVX2,
    CPU_FEATURE_XAPIC,
    CPU_FEATURE_X2APIC,
    CPU_FEATURE_POPCNT,
    CPU_FEATURE_AES,
    CPU_FEATURE_NX,
    CPU_FEATURE_XSAVE,
    CPU_FEATURE_OSXSAVE,
} cpu_feature_t;

// Size (in bytes) of the contiguous XSAVE state area the CPU requires.
// Valid only when XSAVE is supported (see cpu_has_feature(CPU_FEATURE_XSAVE));
// callers should fall back to a 512-byte FXSAVE area otherwise.
size_t xsave_area_size(void);

// Bitmask of XSAVE state components (CPUID leaf 0xD subleaf 0 EDX:EBX) that
// are supported by the CPU.  Bit 0 = x87, bit 1 = SSE, bit 2 = AVX, etc.
uint64_t xsave_feature_mask(void);

#define CPUID_VENDOR_INTEL "GenuineIntel"
#define CPUID_VENDOR_AMD "AuthenticAMD"
#define CPUID_VENDOR_AMD_OLD "AMDisbetter!"
#define CPUID_VENDOR_KVM " KVMKVMKVM  "
#define CPUID_VENDOR_QEMU "TCGTCGTCGTCG"
#define CPUID_VENDOR_VIRTUALBOX "VBoxVBoxVBox"
#define CPUID_VENDOR_VMWARE "VMwareVMware"

const char* get_cpu_name(void);
const char* get_cpu_vendor(void);
uint32_t get_cpu_family(void);
uint32_t get_cpu_model(void);
uint32_t get_cpu_stepping(void);
uint32_t get_cpu_cores(void);
uint32_t get_cpu_threads(void);
uint32_t get_cpu_freq(void);
bool cpu_has_feature(cpu_feature_t feature);
void cache_machine_info(void);