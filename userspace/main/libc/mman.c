#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdint.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    int64_t ret;
    register int64_t r10 asm("r10") = (int64_t)flags;
    register int64_t r8  asm("r8")  = (int64_t)fd;
    register int64_t r9  asm("r9")  = (int64_t)offset;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"((int64_t)SYS_mmap),
          "D"((int64_t)addr),
          "S"((int64_t)length),
          "d"((int64_t)prot),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return (ret < 0) ? MAP_FAILED : (void *)ret;
}

int mprotect(void *addr, size_t length, int prot) {
    int64_t ret;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"((int64_t)SYS_mprotect),
          "D"((int64_t)addr),
          "S"((int64_t)length),
          "d"((int64_t)prot)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

int munmap(void *addr, size_t length) {
    int64_t ret;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"((int64_t)SYS_munmap),
          "D"((int64_t)addr),
          "S"((int64_t)length)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}
