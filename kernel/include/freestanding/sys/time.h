#pragma once

#include <freestanding/stdint.h>

typedef long suseconds_t;

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};