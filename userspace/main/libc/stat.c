#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

int mkdir(const char *path, mode_t mode) {
    return (int)syscall(SYS_mkdir, path, mode);
}