#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__i386__)
#define __PRI64 "ll"
#define __PRIMAX "ll"
#elif defined(__x86_64__)
#define __PRI64 "l"
#define __PRIMAX "l"
#else
#error "Unsupported architecture for inttypes.h."
#endif

#define PRId8 "d"
#define PRIi8 "i"
#define PRIo8 "o"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRIX8 "X"

#define PRId16 "d"
#define PRIi16 "i"
#define PRIo16 "o"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"

#define PRId32 "d"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"

#define PRId64 __PRI64 "d"
#define PRIi64 __PRI64 "i"
#define PRIo64 __PRI64 "o"
#define PRIu64 __PRI64 "u"
#define PRIx64 __PRI64 "x"
#define PRIX64 __PRI64 "X"

#define PRIdMAX __PRIMAX "d"
#define PRIiMAX __PRIMAX "i"
#define PRIoMAX __PRIMAX "o"
#define PRIuMAX __PRIMAX "u"
#define PRIxMAX __PRIMAX "x"
#define PRIXMAX __PRIMAX "X"

#define PRIdPTR __PRIMAX "d"
#define PRIiPTR __PRIMAX "i"
#define PRIoPTR __PRIMAX "o"
#define PRIuPTR __PRIMAX "u"
#define PRIxPTR __PRIMAX "x"
#define PRIXPTR __PRIMAX "X"

#define SCNd8 "hhd"
#define SCNi8 "hhi"
#define SCNo8 "hho"
#define SCNu8 "hhu"
#define SCNx8 "hhx"

#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNo16 "ho"
#define SCNu16 "hu"
#define SCNx16 "hx"

#define SCNd32 "d"
#define SCNi32 "i"
#define SCNo32 "o"
#define SCNu32 "u"
#define SCNx32 "x"

#define SCNd64 __PRI64 "d"
#define SCNi64 __PRI64 "i"
#define SCNo64 __PRI64 "o"
#define SCNu64 __PRI64 "u"
#define SCNx64 __PRI64 "x"

#define SCNdMAX __PRIMAX "d"
#define SCNiMAX __PRIMAX "i"
#define SCNoMAX __PRIMAX "o"
#define SCNuMAX __PRIMAX "u"
#define SCNxMAX __PRIMAX "x"

#define SCNdPTR __PRIMAX "d"
#define SCNiPTR __PRIMAX "i"
#define SCNoPTR __PRIMAX "o"
#define SCNuPTR __PRIMAX "u"
#define SCNxPTR __PRIMAX "x"

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);
intmax_t strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);
intmax_t wcstoimax(const wchar_t *nptr, wchar_t **endptr, int base);
uintmax_t wcstoumax(const wchar_t *nptr, wchar_t **endptr, int base);

#ifdef __cplusplus
}
#endif
