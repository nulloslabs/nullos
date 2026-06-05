#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

int stat(const char *path, struct stat *buf) {
    return (int)syscall(SYS_stat, path, buf);
}

int fstat(int fd, struct stat *buf) {
    return (int)syscall(SYS_fstat, fd, buf);
}

int chmod(const char *path, mode_t mode) {
    return (int)syscall(SYS_chmod, path, mode);
}

int fchmod(int fd, mode_t mode) {
    return (int)syscall(SYS_fchmod, fd, mode);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    return (int)syscall(SYS_fchmodat, dirfd, path, mode, flags);
}

int mkdir(const char *path, mode_t mode) {
    return (int)syscall(SYS_mkdir, path, mode);
}
