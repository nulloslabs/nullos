#include <sys/times.h>
#include <sys/syscall.h>
#include <unistd.h>

clock_t times(struct tms *buf) { return (clock_t)syscall(SYS_times, buf); }