#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <times.h>
#include <termios.h>
#include <limits.h>
#include <errno.h>
#include <asm/unistd.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/futex.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <main/log.h>
#include <main/spinlocks.h>
#include <main/halt.h>
#include <main/domainname.h>
#include <main/timekeeping.h>
#include <main/mp.h>
#include <main/fd.h>
#include <main/signal.h>
#include <main/rng.h>
#include <io/fonts.h>
#include <io/devtmpfs.h>
#include <io/pts_devices.h>
#include <io/terminal.h>
#include <io/tty.h>
#include <io/time.h>
#include <io/sockets.h>
#include <io/unix_sockets.h>
#include <io/procfs.h>
#include <io/ext4.h>
#include <io/vfat.h>
#include <io/gpt.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/time.h>

void sys_nanosleep(syscall_frame_t *frame) {
    frame->rax = (uint64_t)do_clock_nanosleep(CLOCK_MONOTONIC, 0, (const struct timespec *)frame->rdi, (struct timespec *)frame->rsi);
}

void sys_clock_nanosleep(syscall_frame_t *frame) {
    frame->rax = (uint64_t)do_clock_nanosleep((int)frame->rdi, (int)frame->rsi, (const struct timespec *)frame->rdx, (struct timespec *)frame->r10);
}

void sys_getitimer(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    struct itimerval *user_value = (struct itimerval *)frame->rsi;
    struct itimerval value;

    if (which != ITIMER_REAL) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_value || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_value, sizeof(value))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    update_interval_timers();
    fill_real_itimer(current_task_ptr, &value);
    copy_to_user(user_value, &value, sizeof(value));
    frame->rax = 0;
}

void sys_setitimer(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    const struct itimerval *user_new = (const struct itimerval *)frame->rsi;
    struct itimerval *user_old = (struct itimerval *)frame->rdx;
    struct itimerval new_value;
    struct itimerval old_value;
    uint64_t value_us;
    uint64_t interval_us;
    uint64_t deadline_us = 0;

    if (which != ITIMER_REAL) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_new || copy_from_user(&new_value, user_new, sizeof(new_value)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (user_old && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_old, sizeof(old_value))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (timeval_to_us(&new_value.it_value, &value_us) < 0 || timeval_to_us(&new_value.it_interval, &interval_us) < 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    update_interval_timers();
    fill_real_itimer(current_task_ptr, &old_value);

    if (value_us != 0) {
        uint64_t now = time_get_realtime_us();
        if (value_us > UINT64_MAX - now) { frame->rax = (uint64_t)-EINVAL; return; }
        deadline_us = now + value_us;
    }
    current_task_ptr->real_timer_interval_us = interval_us;
    current_task_ptr->real_timer_deadline_us = deadline_us;

    if (user_old) copy_to_user(user_old, &old_value, sizeof(old_value));
    frame->rax = 0;
}

void sys_time(syscall_frame_t *frame) {
    time_t *result = (time_t *)frame->rdi;
    time_t seconds = (time_t)(time_get_realtime_us() / 1000000ULL);

    if (result) {
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)result, sizeof(*result))) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)result, &seconds, sizeof(seconds)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    frame->rax = (uint64_t)seconds;
}

void sys_gettimeofday(syscall_frame_t *frame) {
    struct timeval *tv = (struct timeval *)frame->rdi;
    struct timezone *tz = (struct timezone *)frame->rsi;

    if (tv && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)tv, sizeof(*tv))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (tz && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)tz, sizeof(*tz))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    if (tv) {
        uint64_t usec = time_get_realtime_us();

        struct timeval ktv;
        ktv.tv_sec = (time_t)(usec / 1000000ULL);
        ktv.tv_usec = (suseconds_t)(usec % 1000000ULL);
        if (write_vmm(current_task_ptr->ctx, (uint64_t)tv, &ktv, sizeof(ktv)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    if (tz) { struct timezone ktz = {0}; if (write_vmm(current_task_ptr->ctx, (uint64_t)tz, &ktz, sizeof(ktz)) < 0) { frame->rax = (uint64_t)-EFAULT; return; } }

    frame->rax = 0;
}

void sys_times(syscall_frame_t *frame) {
    tms_t *buf = (tms_t *)frame->rdi;
    // TODO: use real per-task CPU tick accounting for tms_utime/tms_stime.
    // Previously tms_stime was set to system uptime ticks, making sys time
    // appear equal to real elapsed time for any blocking/sleeping process.
    uint64_t elapsed_ticks = get_monotonic_time_us() / 10000ULL;

    if (buf) {
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, sizeof(*buf))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        tms_t t = {0};
        // tms_utime and tms_stime remain 0 until per-task CPU accounting is added
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, &t, sizeof(t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    frame->rax = (uint64_t)elapsed_ticks;
}

void sys_settimeofday(syscall_frame_t *frame) {
    const struct timeval *tv = (const struct timeval *)frame->rdi;
    const struct timezone *tz = (const struct timezone *)frame->rsi;

    if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }

    if (tz) {
        struct timezone ktz;
        if (copy_from_user(&ktz, tz, sizeof(ktz)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ktz.tz_minuteswest < -15 * 60 || ktz.tz_minuteswest > 15 * 60) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    if (!tv) { frame->rax = 0; return; }

    struct timeval ktv;
    if (copy_from_user(&ktv, tv, sizeof(ktv)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    if (ktv.tv_sec < 0 || ktv.tv_usec < 0 || ktv.tv_usec >= 1000000 || ktv.tv_sec > (TIME_T_MAX / 1000000)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t desired_us = ((uint64_t)ktv.tv_sec * 1000000ULL) + (uint64_t)ktv.tv_usec;
    time_set_realtime_us(desired_us);

    frame->rax = 0;
}

void sys_clock_gettime(syscall_frame_t *frame) {
    int clk_id   = (int)frame->rdi;
    struct timespec *tp = (struct timespec *)frame->rsi;

    if (!tp || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)tp, sizeof(struct timespec))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    uint64_t us;
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        us = time_get_realtime_us();
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
        us = get_monotonic_time_us();
        break;
    default:
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    struct timespec ts = {
        .tv_sec  = (time_t)(us / 1000000ULL), .tv_nsec = (long)(us % 1000000ULL) * 1000L, };
    if (write_vmm(current_task_ptr->ctx, (uint64_t)tp, &ts, sizeof(ts)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_clock_getres(syscall_frame_t *frame) {
    int clk_id = (int)frame->rdi;
    struct timespec *resolution = (struct timespec *)frame->rsi;

    switch (clk_id) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_BOOTTIME:
            break;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }

    if (!resolution) {
        frame->rax = 0;
        return;
    }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)resolution, sizeof(*resolution))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    struct timespec result = { .tv_sec = 0, .tv_nsec = 1000 };
    if (write_vmm(current_task_ptr->ctx, (uint64_t)resolution, &result, sizeof(result)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}
