#pragma once

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6
#define FUTEX_UNLOCK_PI         7
#define FUTEX_TRYLOCK_PI        8
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10
#define FUTEX_WAIT_REQUEUE_PI   11
#define FUTEX_CMP_REQUEUE_PI    12
#define FUTEX_LOCK_PI2          13

#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

#define FUTEX_BITSET_MATCH_ANY  0xFFFFFFFFU

#define FUTEX_OP_SET            0
#define FUTEX_OP_ADD            1
#define FUTEX_OP_OR             2
#define FUTEX_OP_ANDN           3
#define FUTEX_OP_XOR            4
#define FUTEX_OP_ARG_SHIFT      8

#define FUTEX_OP_CMP_EQ         0
#define FUTEX_OP_CMP_NE         1
#define FUTEX_OP_CMP_LT         2
#define FUTEX_OP_CMP_LE         3
#define FUTEX_OP_CMP_GT         4
#define FUTEX_OP_CMP_GE         5

#define FUTEX_WAITERS           0x80000000
#define FUTEX_OWNER_DIED        0x40000000
#define FUTEX_TID_MASK          0x3FFFFFFF
