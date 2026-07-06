#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/asm/signal.h>
#include <freestanding/bits/types/siginfo_t.h>
#include <freestanding/bits/sigaction.h>
#include <freestanding/bits/siginfo-consts.h>

#define SS_ONSTACK    1
#define SS_DISABLE    2
#define SS_AUTODISARM 0x80000000U
