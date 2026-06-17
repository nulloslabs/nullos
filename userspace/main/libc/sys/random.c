#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/syscall.h>

int getrandom(void *buf, size_t buflen, unsigned int flags) {
    return (int)syscall(SYS_getrandom, buf, buflen, flags);
}
