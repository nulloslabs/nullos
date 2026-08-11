#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <bits/time.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
