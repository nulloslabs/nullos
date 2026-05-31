#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WCHAR_T
#define _WCHAR_T
typedef unsigned int wchar_t;
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN (-2147483647 - 1)
#endif

#ifndef WCHAR_MAX
#define WCHAR_MAX 2147483647
#endif

long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base);

#ifdef __cplusplus
}
#endif


