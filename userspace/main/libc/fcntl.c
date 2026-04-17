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