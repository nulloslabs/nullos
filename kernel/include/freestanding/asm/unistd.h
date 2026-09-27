#pragma once

#if defined(__x86_64__)
#include <freestanding/asm/unistd_64.h>
#else
#error "Unsupported architecture for freestanding/asm/unistd.h."
#endif
