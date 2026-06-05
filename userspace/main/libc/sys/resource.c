#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

int getrlimit(int resource, struct rlimit *rlim) {
    return (int)syscall(SYS_getrlimit, resource, rlim);
}

int setrlimit(int resource, const struct rlimit *rlim) {
    return (int)syscall(SYS_setrlimit, resource, rlim);
}

int getrusage(int who, struct rusage *usage) {
    return (int)syscall(SYS_getrusage, who, usage);
}
