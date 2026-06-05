#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

int uname(struct utsname *buf) {
    return (int)syscall(SYS_uname, buf);
}
