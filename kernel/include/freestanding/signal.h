#pragma once

#include <stdint.h>
#include <stddef.h>
#include <asm/signal.h>
#include <bits/types/siginfo_t.h>
#include <bits/sigaction.h>
#include <bits/siginfo-consts.h>

#define SS_ONSTACK    1
#define SS_DISABLE    2
#define SS_AUTODISARM 0x80000000U
