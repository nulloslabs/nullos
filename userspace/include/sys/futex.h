#pragma once

#include <stdint.h>

/*
 * Futex operations (lower bits of the op argument to syscall(SYS_futex, ...))
 */
#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2   /* obsolete */
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10

/* Modifier flags OR-ed into the op argument */
#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

/* Convenience: private variants (no cross-process sharing needed) */
#define FUTEX_WAIT_PRIVATE      (FUTEX_WAIT     | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE      (FUTEX_WAKE     | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE   (FUTEX_REQUEUE  | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PRIVATE (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_OP_PRIVATE   (FUTEX_WAKE_OP  | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_BITSET_PRIVATE (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG)

/* Bitset that matches every waiter */
#define FUTEX_BITSET_MATCH_ANY  0xFFFFFFFFU

/* FUTEX_WAKE_OP encoded operation types (bits 28–31 of val3) */
#define FUTEX_OP_SET            0
#define FUTEX_OP_ADD            1
#define FUTEX_OP_OR             2
#define FUTEX_OP_ANDN           3
#define FUTEX_OP_XOR            4
#define FUTEX_OP_ARG_SHIFT      8   /* flag: shift op_arg left */

/* FUTEX_WAKE_OP comparison types (bits 24–27 of val3) */
#define FUTEX_OP_CMP_EQ         0
#define FUTEX_OP_CMP_NE         1
#define FUTEX_OP_CMP_LT         2
#define FUTEX_OP_CMP_LE         3
#define FUTEX_OP_CMP_GT         4
#define FUTEX_OP_CMP_GE         5

/*
 * Encode a FUTEX_WAKE_OP val3 argument.
 * op/cmp are FUTEX_OP_ and FUTEX_OP_CMP_ values; oparg/cmparg are the operands.
 */
#define FUTEX_OP(op, oparg, cmp, cmparg) \
    (((op  & 0xf) << 28) | ((cmp & 0xf) << 24) | \
     ((oparg & 0xfff) << 12) | (cmparg & 0xfff))
