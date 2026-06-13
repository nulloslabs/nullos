#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <unistd.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    int64_t ret = syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    return (ret < 0) ? MAP_FAILED : (void *)ret;
}

int mprotect(void *addr, size_t length, int prot) {
    return (int)syscall(SYS_mprotect, addr, length, prot);
}

int munmap(void *addr, size_t length) {
    return (int)syscall(SYS_munmap, addr, length);
}