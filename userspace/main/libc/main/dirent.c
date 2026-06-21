#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/syscall.h>

int getdents(int fd, struct dirent *buf, int count) { return (int)syscall(SYS_getdents, fd, buf, count); }

int getdents64(int fd, struct dirent *buf, size_t count) { return (int)syscall(SYS_getdents64, fd, buf, count); }

DIR *fdopendir(int fd) {
    if (fd < 0) return NULL;

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) return NULL;

    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    return dir;
}

DIR *opendir(const char *name) {
    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;

    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return NULL; }

    return dir;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return NULL;

    if (dirp->buf_pos >= dirp->buf_end) {
        int n = getdents(dirp->fd, (struct dirent *)dirp->buf, sizeof(dirp->buf));
        if (n <= 0) return NULL;
        dirp->buf_pos = 0;
        dirp->buf_end = n;
    }

    struct dirent *de = (struct dirent *)(dirp->buf + dirp->buf_pos);
    dirp->buf_pos += de->d_reclen;
    return de;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    int ret = close(dirp->fd);
    free(dirp);
    return ret;
}