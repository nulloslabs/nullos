#pragma once

#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>
#include <freestanding/bits/time.h>

#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
