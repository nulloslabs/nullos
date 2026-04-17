#pragma once

#if defined(__i386__)
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned long uint32_t;
typedef long int32_t;
typedef unsigned int uintptr_t;
typedef int intptr_t;
typedef unsigned long long uintmax_t;
typedef long long intmax_t;
#elif defined(__x86_64__)
typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long uintptr_t;
typedef long intptr_t;
typedef unsigned long uintmax_t;
typedef long intmax_t;
#else
#error "Unsupported architecture for stdint.h."
#endif

typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef signed char int8_t;