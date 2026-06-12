#include <sys/wait.h>
#include <unistd.h>
#include <sys/syscall.h>

pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage) { return (pid_t)syscall(SYS_wait4, pid, wstatus, options, rusage); }

pid_t wait(int *wstatus) { return wait4(-1, wstatus, 0, NULL); }

pid_t waitpid(pid_t pid, int *wstatus, int options) { return wait4(pid, wstatus, options, NULL); }