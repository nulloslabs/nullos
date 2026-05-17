#include <unistd.h>
#include <sys/syscall.h>

int main() {
    long fd2 = syscall(SYS_dup, 1); // SYS_dup = 11, dup stdout
    syscall(SYS_write, fd2, "hello from dup\n", 15); // SYS_write = 4
    syscall(SYS_close, fd2); // SYS_close = 2
    return 0;
}
