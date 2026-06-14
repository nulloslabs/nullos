#pragma once

/*
 * Kernel-internal futex interface.
 *
 * futex_check_timeouts() is called from schedule() on every timer tick.
 * It expires timed-out FUTEX_WAIT callers and also wakes stopped waiters
 * that have received a pending signal (so that -EINTR can be returned).
 */
void futex_check_timeouts(void);
