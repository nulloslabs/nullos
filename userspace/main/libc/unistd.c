#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

// Variables, structs etc. for functions
static void *current_program_break = NULL;
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
    register int64_t r8 asm("r8") = a5;
    register int64_t r9 asm("r9") = a6;
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
    if (ret < 0) { 
        // Uh oh! We hit an error, save our error code inside errno and return -1
        errno = (int)-ret;
        return -1;
    }
    // Great! We didn't hit an error, just return our return value given by the syscall
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
    int allocated = 0; // Track who owns the buffer

    if (buf == NULL) {
        if (size == 0) size = PATH_MAX;
        buf = malloc(size);
        if (!buf) return NULL;
        allocated = 1; // We allocated it, so we are responsible for it
    }

    int64_t ret = syscall(SYS_getcwd, buf, size);

    if (ret < 0) {
        // ONLY free the buffer if WE allocated it. 
        // If the user provided it, leave it alone!
        if (allocated) {
            free(buf);
        }
        return NULL;
    }

    return (char *)ret; 
}

int chdir(const char *path) {
    return (int)syscall(SYS_chdir, path);
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

pid_t waitpid(pid_t pid, int *wstatus, int options) {
    return (pid_t)syscall(SYS_waitpid, pid, wstatus, options);
}

pid_t getpid(void) {
    return (pid_t)syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
}

pid_t getppid(void) {
    return (pid_t)syscall(SYS_getppid, 0, 0, 0, 0, 0, 0);
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

int reboot(int how) {
    // NOTE: Shouldn't return anything if it was successful, only if we got an error.
    // NOTE 2: This dosen't match POSIX, mostly matches BSD but not exactly.
    return (int)syscall(SYS_reboot, how);
}
