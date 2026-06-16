#include <sys/syscall.h>
#include <asm/prctl.h>

long arch_prctl(int code, unsigned long addr) {
    // We CAN'T under ANY circumstance, use syscall(), it...crashes...with a page fault (probably because of stdarg)
    long ret;

    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_arch_prctl), "D"(code), "S"(addr)
        : "rcx", "r11", "memory"
    );

    if (ret < 0) { 
        errno = (int)-ret;
        return -1;
    }

    return ret;
}
