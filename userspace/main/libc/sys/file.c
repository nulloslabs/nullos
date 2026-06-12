#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

int flock(int fd, int operation) { return (int)syscall(SYS_flock, fd, operation); }