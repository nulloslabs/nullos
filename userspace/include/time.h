#pragma once

#include <stdint.h>
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

int nanosleep(const struct timespec *req, struct timespec *rem);
int usleep(unsigned int usec);
