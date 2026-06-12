#pragma once

#if defined(__i386__)
typedef unsigned long long uint64_t;
typedef unsigned long uint32_t;
typedef long long int64_t;
typedef long int32_t;
typedef int intptr_t;
typedef unsigned int uintptr_t;
typedef long long intmax_t;
typedef unsigned long long uintmax_t;
#elif defined(__x86_64__)
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef long int64_t;
typedef int int32_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef long intmax_t;
typedef unsigned long uintmax_t;
#else
#error "Unsupported architecture for stdint.h."
#endif

typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef signed char int8_t;

#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255U

#define INT16_MIN (-32767 - 1)
#define INT16_MAX 32767
#define UINT16_MAX 65535U

#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U

#if defined(__i386__)
#define INT64_MIN (-9223372036854775807LL - 1LL)
#define INT64_MAX 9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL
#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#elif defined(__x86_64__)
#define INT64_MIN (-9223372036854775807L - 1L)
#define INT64_MAX 9223372036854775807L
#define UINT64_MAX 18446744073709551615UL
#define INTPTR_MIN INT64_MIN
#define INTPTR_MAX INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#endif