#include <utime.h>
#include <sys/syscall.h>
#include <unistd.h>

int utime(const char *filename, const struct utimbuf *times) { return (int)syscall(SYS_utime, filename, times); }