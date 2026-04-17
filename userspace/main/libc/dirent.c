#include <unistd.h>
#include <dirent.h>
#include <sys/syscall.h>

int getdents(int fd, struct dirent *buf, int count) {
    return (int)syscall(SYS_getdents, fd, buf, count);
}