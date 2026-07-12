#pragma once

#include <freestanding/stdint.h>

void logat(const char* file, const char *fmt, ...);
void serial_logat(const char* file, uint16_t port, const char *fmt, ...);

#define log(fmt, ...) logat(__FILE__, fmt, ##__VA_ARGS__)
#define serial_log(port, fmt, ...) logat(__FILE__, port, fmt, ##__VA_ARGS__)
