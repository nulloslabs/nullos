#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/uio.h>

// Variables, structs etc. for functions
static void *current_program_break = NULL;
const char *__progname = NULL;
char **environ = NULL;

// Helpers for functions
static const char *get_env_value(const char *name) {
    if (!environ || !name) return NULL;

    size_t name_len = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, name_len) == 0 && environ[i][name_len] == '=') {
            return environ[i] + name_len + 1;
        }
    }

    return NULL;
}

static int has_slash(const char *s) {
    return strchr(s, '/') != NULL;
}

__attribute__((naked)) int64_t syscall(int64_t num, ...) {
    __asm__ volatile (
        "movq %rdi, %rax\n"
        "movq %rsi, %rdi\n"
        "movq %rdx, %rsi\n"
        "movq %rcx, %rdx\n"
        "movq %r8,  %r10\n"
        "movq %r9,  %r8\n"
        "movq 8(%rsp), %r9\n"
        "syscall\n"
        "cmpq $-4095, %rax\n"
        "jae .Lerror\n"
        "ret\n"
        ".Lerror:\n"
        "negq %rax\n"
        "movq %rax, %rcx\n"
#ifdef __PIC__
        "movq errno@GOTPCREL(%rip), %rdx\n"
        "movl %ecx, (%rdx)\n"
#else
        "movl %ecx, errno(%rip)\n"
#endif
        "movq $-1, %rax\n"
        "ret\n"
    );
}

__attribute__((noreturn)) void _exit(int status) {
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}

int close(int fd) {
    return (int)syscall(SYS_close, fd);
}

int pipe(int pipefd[2]) {
    return (int)syscall(SYS_pipe, pipefd);
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall(SYS_read, fd, (int64_t)buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall(SYS_write, fd, (int64_t)buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return (ssize_t)syscall(SYS_readv, fd, (int64_t)iov, iovcnt);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return (ssize_t)syscall(SYS_writev, fd, (int64_t)iov, iovcnt);
}

char *getcwd(char *buf, size_t size) {
    int allocated = 0; // Track who owns the buffer

    if (buf == NULL) {
        if (size == 0) size = PATH_MAX;
        buf = malloc(size);
        if (!buf) return NULL;
        allocated = 1; // We allocated it, so we are responsible for it
    }

    int64_t ret = syscall(SYS_getcwd, buf, size);

    if (ret < 0) {
        if (allocated) free(buf);
        return NULL;
    }

    return buf; 
}

int chdir(const char *path) {
    return (int)syscall(SYS_chdir, path);
}



int rmdir(const char *pathname) {
    return (int)syscall(SYS_rmdir, pathname);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)syscall(SYS_execve, path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    if (!file || !*file) return execv(file, argv);
    if (has_slash(file)) return execv(file, argv);

    const char *path = get_env_value("PATH");
    if (!path || !*path) path = "/bin:/usr/bin";

    int last_ret = -1;
    while (1) {
        char full[PATH_MAX];
        int pos = 0;

        while (path[pos] && path[pos] != ':') pos++;

        if (pos == 0) {
            strncpy(full, file, sizeof(full) - 1);
            full[sizeof(full) - 1] = '\0';
        } else {
            size_t dir_len = (size_t)pos;
            if (dir_len >= sizeof(full)) dir_len = sizeof(full) - 1;

            strncpy(full, path, dir_len);
            full[dir_len] = '\0';

            if (dir_len > 0 && full[dir_len - 1] != '/') {
                strncat(full, "/", sizeof(full) - strlen(full) - 1);
            }
            strncat(full, file, sizeof(full) - strlen(full) - 1);
        }

        last_ret = execv(full, argv);

        if (!path[pos]) break;
        path += pos + 1;
    }

    return last_ret;
}

pid_t fork(void) {
    return (pid_t)syscall(SYS_fork, 0, 0, 0, 0, 0, 0);
}

pid_t getpid(void) {
    return (pid_t)syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
}

pid_t getppid(void) {
    return (pid_t)syscall(SYS_getppid, 0, 0, 0, 0, 0, 0);
}

pid_t getpgid(pid_t pid) {
    return (pid_t)syscall(SYS_getpgid, (int64_t)pid);
}

int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall(SYS_setpgid, (int64_t)pid, (int64_t)pgid);
}

pid_t setsid(void) {
    return (pid_t)syscall(SYS_setsid);
}

pid_t getsid(pid_t pid) {
    return (pid_t)syscall(SYS_getsid, (int64_t)pid);
}

pid_t tcgetpgrp(int fd) {
    pid_t pgrp = 0;
    if (syscall(SYS_ioctl, (int64_t)fd, (int64_t)0x540F /* TIOCGPGRP */, (int64_t)&pgrp) < 0)
        return -1;
    return pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp) {
    return (int)syscall(SYS_ioctl, (int64_t)fd, (int64_t)0x5410 /* TIOCSPGRP */, (int64_t)&pgrp);
}

uid_t getuid(void) {
    return (uid_t)syscall(SYS_getuid, 0, 0, 0, 0, 0, 0);
}

gid_t getgid(void) {
    return (gid_t)syscall(SYS_getgid, 0, 0, 0, 0, 0, 0);
}

uid_t geteuid(void) {
    return (uid_t)syscall(SYS_geteuid, 0, 0, 0, 0, 0, 0);
}

gid_t getegid(void) {
    return (gid_t)syscall(SYS_getegid, 0, 0, 0, 0, 0, 0);
}

int setuid(uid_t uid) {
    return (int)syscall(SYS_setuid, uid);
}

int setgid(gid_t gid) {
    return (int)syscall(SYS_setgid, gid);
}

int seteuid(uid_t euid) {
    return (int)syscall(SYS_seteuid, euid);
}

int setegid(gid_t egid) {
    return (int)syscall(SYS_setegid, egid);
}

int brk(void *addr) {
    return (int)syscall(SYS_brk, addr);
}

void *sbrk(intptr_t increment) {
    if (current_program_break == NULL) current_program_break = (void *)(uintptr_t)brk(NULL);
    void *prev = current_program_break;
    void *new_break = (void *)((uintptr_t)current_program_break + increment);
    current_program_break = (void *)(uintptr_t)brk(new_break);
    return prev;
}

int gethostname(char *name, size_t size) {
    return (int)syscall(SYS_gethostname, name, size);
}

int sethostname(const char *name, size_t size) {
    return (int)syscall(SYS_sethostname, name, size);
}

int reboot(int howto) {
    return (int)syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, howto, 0);
}

int64_t lseek(int fd, int64_t offset, int whence) {
    return (int64_t)syscall(SYS_lseek, fd, offset, whence);
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)syscall(SYS_readlink, path, buf, bufsiz);
}

int unlink(const char *path) {
    return (int)syscall(SYS_unlink, path);
}

int symlink(const char *target, const char *linkpath) {
    return (int)syscall(SYS_symlink, target, linkpath);
}

int chown(const char *path, uid_t owner, gid_t group) {
    return (int)syscall(SYS_chown, path, owner, group);
}

int lchown(const char *path, uid_t owner, gid_t group) {
    return (int)syscall(SYS_lchown, path, owner, group);
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    return (int)syscall(SYS_unlinkat, dirfd, pathname, flags);
}

int symlinkat(const char *target, int newdirfd, const char *linkpath) {
    return (int)syscall(SYS_symlinkat, target, newdirfd, linkpath);
}

int dup(int oldfd) {
    return (int)syscall(SYS_dup, oldfd);
}

int dup2(int oldfd, int newfd) {
    return (int)syscall(SYS_dup2, oldfd, newfd);
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req, rem;
    req.tv_sec = seconds;
    req.tv_nsec = 0;
    if (syscall(SYS_nanosleep, (int64_t)&req, (int64_t)&rem) < 0) {
        return rem.tv_sec;
    }
    return 0;
}
