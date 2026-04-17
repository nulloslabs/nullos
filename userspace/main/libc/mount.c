#include <sys/syscall.h>
#include <unistd.h>
#include <sys/mount.h>

int mount(const char *source, const char *target, const char *filesystemtype, unsigned long mountflags, const void *data) {
     return (int)syscall(SYS_mount, source, target, filesystemtype, mountflags, data);
}

int umount(const char *target, int flags) {
     return (int)syscall(SYS_umount, target, flags);
}