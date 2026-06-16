#include <sys/syscall.h>
#include <asm/prctl.h>
#include <unistd.h>

long arch_prctl(int code, unsigned long addr) {
    return syscall(SYS_arch_prctl, code, addr);
}
