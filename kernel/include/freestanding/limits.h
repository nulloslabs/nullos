#pragma once

#include <freestanding/stddef.h>

#define CHAR_BIT    8
#define CHAR_MIN    (-128)
#define CHAR_MAX    127
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255U

#define SHRT_MIN    (-32767 - 1)
#define SHRT_MAX    32767
#define USHRT_MAX   65535U

#define WORD_BIT    32
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_BIT    64
#define LONG_MIN    (-9223372036854775807L - 1L)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL

#define LLONG_MIN   (-9223372036854775807LL - 1LL)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

#define SSIZE_MAX   ((ssize_t)(SIZE_MAX / 2))

#define TIME_T_MIN  ((time_t)(-TIME_T_MAX - 1))
#define TIME_T_MAX  ((time_t)(((uint64_t)1 << ((sizeof(time_t) * 8) - 1)) - 1))

#define OFF_MAX     LLONG_MAX
#define OFF_MIN     LLONG_MIN

#define MB_LEN_MAX  4

#define PATH_MAX    4096
#define NAME_MAX    255

#define NGROUPS_MAX     65536
#define ARG_MAX         131072
#define CHILD_MAX       1024
#define OPEN_MAX        1024
#define STREAM_MAX      16
#define TZNAME_MAX      6
#define LOGIN_NAME_MAX  256
#define HOST_NAME_MAX   255
#define TTY_NAME_MAX    32
#define SYMLINK_MAX     4096
#define SYMLOOP_MAX     40
#define LINK_MAX        65000
#define MAX_CANON       255
#define MAX_INPUT       255
#define PIPE_BUF        4096
