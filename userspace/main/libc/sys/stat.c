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

int lstat(const char *path, struct stat *buf) {
    return (int)syscall(SYS_lstat, path, buf);
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    return (int)syscall(SYS_fstatat, dirfd, pathname, buf, flags);
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    return (int)syscall(SYS_mknod, path, mode, dev);
}

mode_t umask(mode_t mask) {
    return (mode_t)syscall(SYS_umask, mask);
}
