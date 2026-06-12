#include <stddef.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

int open(const char *path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return syscall(SYS_open, path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return syscall(SYS_openat, dirfd, path, flags, mode);
}

int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

int fcntl(int fd, int cmd, ...) {
    va_list args;
    va_start(args, cmd);
    uint64_t arg = va_arg(args, uint64_t);
    va_end(args);
    return (int)syscall(SYS_fcntl, fd, cmd, arg);
}