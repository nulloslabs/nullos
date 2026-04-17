#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("\033[2J\033[H");
    if (mount("devfs", "/dev", "devfs", 0, NULL) < 0) {
        perror("Init: mount() failed");
        return 1;
    }

    char *login_argv[] = { "/usr/bin/login", NULL };
    char *login_envp[] = { NULL };

    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child — become login
            execve("/usr/bin/login", login_argv, login_envp);
            perror("Init: execve() failed");
            return 1;
        } else if (pid > 0) {
            // Parent — wait for login to exit then restart it
            wait(NULL);
        } else {
            perror("Init: fork() failed");
            return 1;
        }
    }
    return 0;
}