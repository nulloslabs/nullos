#pragma once

#include <stddef.h>

#define GRND_RANDOM   0x0001
#define GRND_NONBLOCK 0x0002
#define GRND_INSECURE 0x0004

int getrandom(void *buf, size_t buflen, unsigned int flags);
