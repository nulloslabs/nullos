#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <bits/time.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
