#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)syscall(SYS_nanosleep, req, rem);
}

int usleep(unsigned int usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}
