#include <time.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

int nanosleep(const struct timespec *req, struct timespec *rem) { return (int)syscall(SYS_nanosleep, req, rem); }

int usleep(unsigned int usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}

int gettimeofday(struct timeval *tv, struct timezone *tz) { return (int)syscall(SYS_gettimeofday, tv, tz); }

int settimeofday(const struct timeval *tv, const struct timezone *tz) { return (int)syscall(SYS_settimeofday, tv, tz); }

int getitimer(int which, struct itimerval *curr_value) {
    (void)which;
    if (!curr_value) { errno = EINVAL; return -1; }
    curr_value->it_interval.tv_sec = 0;
    curr_value->it_interval.tv_usec = 0;
    curr_value->it_value.tv_sec = 0;
    curr_value->it_value.tv_usec = 0;
    return 0;
}

int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value) {
    (void)which;
    (void)new_value;
    if (old_value) {
        old_value->it_interval.tv_sec = 0;
        old_value->it_interval.tv_usec = 0;
        old_value->it_value.tv_sec = 0;
        old_value->it_value.tv_usec = 0;
    }
    errno = ENOSYS;
    return -1;
}