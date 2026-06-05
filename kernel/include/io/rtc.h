#pragma once

#include <freestanding/stdint.h>

void init_rtc(void);
uint64_t rtc_read_unix_time(void);

