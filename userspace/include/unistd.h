#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern char **environ;

int64_t syscall(int64_t num, ...);
__attribute__((noreturn)) void _exit(int status);
int close(int fd);
int pipe(int pipefd[2]);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
pid_t fork(void);
pid_t waitpid(pid_t pid, int *wstatus, int options);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
gid_t getgid(void);
uid_t geteuid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t euid);
int setegid(gid_t egid);
int kill(pid_t pid, int sig);
int brk(void *addr);
void *sbrk(intptr_t increment);
int gethostname(char *name, size_t size);
int sethostname(const char *name, size_t size);
int reboot(int how);
int64_t lseek(int fd, int64_t offset, int whence);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int unlink(const char *path);
int symlink(const char *target, const char *linkpath);
int chown(const char *path, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int unlinkat(int dirfd, const char *pathname, int flags);

#ifdef __cplusplus
}
#endif
