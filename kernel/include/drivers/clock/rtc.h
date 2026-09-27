#pragma once

#include <freestanding/stdint.h>

uint64_t read_rtc_unix_time(void);
void init_rtc(void);
