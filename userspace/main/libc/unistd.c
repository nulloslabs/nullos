#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

int64_t syscall(int64_t num, ...) {
    va_list args;
    va_start(args, num);

    int64_t a1 = va_arg(args, int64_t);
    int64_t a2 = va_arg(args, int64_t);
    int64_t a3 = va_arg(args, int64_t);
    int64_t a4 = va_arg(args, int64_t);
    int64_t a5 = va_arg(args, int64_t);
    int64_t a6 = va_arg(args, int64_t);
    va_end(args);
    int64_t ret;

    register int64_t r10 asm("r10") = a4;
    register int64_t r8 asm("r8")  = a5;
    register int64_t r9 asm("r9")  = a6;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num),
          "D"(a1),
          "S"(a2),
          "d"(a3),
          "r"(r10),
          "r"(r8),
          "r"(r9)
        : "rcx", "r11", "memory"
    );
    if (ret < 0) errno = (int)-ret;
    return ret;
}

__attribute__((noreturn)) void _exit(int status) {
    syscall(SYS_exit, status);
    __builtin_unreachable();
}

int close(int fd) {
    return (int)syscall(SYS_close, fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall(SYS_read, fd, (int64_t)buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall(SYS_write, fd, (int64_t)buf, count);
}

char *getcwd(char *buf, size_t size) {
    return (char *)syscall(SYS_getcwd, buf, size);
}

int chdir(const char *path) {
    return (int)syscall(SYS_chdir, path);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)syscall(SYS_execve, path, argv, envp);
}

pid_t fork(void) {
    return (pid_t)syscall(SYS_fork);
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
    return (pid_t)syscall(SYS_waitpid, pid, wstatus, options);
}

pid_t getpid(void) {
    return (pid_t)syscall(SYS_getpid);
}

pid_t getppid(void) {
    return (pid_t)syscall(SYS_getppid);
}
