#pragma once

#include <freestanding/sys/types.h>

typedef struct {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
} tms_t;
