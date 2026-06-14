#pragma once

#include <freestanding/stdint.h>

/*
 * Futex operations (lower bits of the op argument to sys_futex).
 */
#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2   /* obsolete – not implemented */
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6   /* PI – not implemented */
#define FUTEX_UNLOCK_PI         7   /* PI – not implemented */
#define FUTEX_TRYLOCK_PI        8   /* PI – not implemented */
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10
#define FUTEX_WAIT_REQUEUE_PI   11  /* PI – not implemented */
#define FUTEX_CMP_REQUEUE_PI    12  /* PI – not implemented */

/* Modifier flags OR-ed into the op argument */
#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

/* Special bitset that matches every waiter */
#define FUTEX_BITSET_MATCH_ANY  0xFFFFFFFFU

/* FUTEX_WAKE_OP encoded operation types (bits 28-31 of val3) */
#define FUTEX_OP_SET            0   /* uaddr2 = oparg */
#define FUTEX_OP_ADD            1   /* uaddr2 += oparg */
#define FUTEX_OP_OR             2   /* uaddr2 |= oparg */
#define FUTEX_OP_ANDN           3   /* uaddr2 &= ~oparg */
#define FUTEX_OP_XOR            4   /* uaddr2 ^= oparg */
#define FUTEX_OP_ARG_SHIFT      8   /* shift the encoded oparg: oparg = 1 << oparg */

/* FUTEX_WAKE_OP comparison types (bits 24-27 of val3) */
#define FUTEX_OP_CMP_EQ         0
#define FUTEX_OP_CMP_NE         1
#define FUTEX_OP_CMP_LT         2
#define FUTEX_OP_CMP_LE         3
#define FUTEX_OP_CMP_GT         4
#define FUTEX_OP_CMP_GE         5
