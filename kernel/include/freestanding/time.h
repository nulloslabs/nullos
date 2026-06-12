#pragma once

#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
