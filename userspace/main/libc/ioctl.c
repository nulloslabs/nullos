#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int ioctl(int fd, unsigned long op, ...) {
    va_list args;
    va_start(args, op);
    void *arg = va_arg(args, void *);
    va_end(args);
    return (int)syscall(SYS_ioctl, fd, op, arg);
}