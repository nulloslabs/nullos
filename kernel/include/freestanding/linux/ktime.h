#pragma once

#include <stdint.h>
#include <linux/types.h>

#define NSEC_PER_SEC  1000000000LL
#define KTIME_MAX     INT64_MAX
#define KTIME_MIN     INT64_MIN
#define KTIME_SEC_MAX (KTIME_MAX / NSEC_PER_SEC)
