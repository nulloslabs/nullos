#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

int64_t syscall(int64_t num, ...);
__attribute__((noreturn)) void _exit(int status);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t fork(void);
pid_t waitpid(pid_t pid, int *wstatus, int options);
pid_t getpid(void);
pid_t getppid(void);
