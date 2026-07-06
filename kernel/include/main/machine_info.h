#pragma once

#include <freestanding/stdint.h>

#define CPUID_VENDOR_INTEL "GenuineIntel"
#define CPUID_VENDOR_INTEL_BUGGY "GenuineIotel" // This is a rare bitflip/typo variant in Intel CPUs.
#define CPUID_VENDOR_AMD "AuthenticAMD"
#define CPUID_VENDOR_AMD_OLD "AMDisbetter!"
#define CPUID_VENDOR_KVM " KVMKVMKVM  "
#define CPUID_VENDOR_QEMU "TCGTCGTCGTCG"
#define CPUID_VENDOR_VIRTUALBOX "VBoxVBoxVBox"
#define CPUID_VENDOR_VMWARE "VMwareVMware"

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

const char* get_cpu_name(void);
const char* get_cpu_vendor(void);
uint32_t get_cpu_family(void);
uint32_t get_cpu_model(void);
uint32_t get_cpu_stepping(void);
uint32_t get_cpu_cores(void);
uint32_t get_cpu_threads(void);
uint32_t get_cpu_freq(void);
size_t get_xsave_area_size(void);
uint64_t get_xsave_feature_mask(void);
bool cpu_has_feature(cpu_feature_t feature);
void cache_machine_info(void);
