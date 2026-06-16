#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage for init:\n");
    printf("  init [OPTIONS]\n\n");
    printf("Options:\n");
    printf("  --help: show help dialouge.\n");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0) { print_usage(); return 0; }

        fprintf(stderr, "init: unknown argument: %s\n", argv[1]);
        return 1;
    }

    printf("\033[2J\033[H");
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0) { perror("init: mount(/dev) failed"); return 1; }
    mkdir("/dev/pts", 0755);
    if (mount("devpts", "/dev/pts", "devpts", 0, NULL) < 0) { perror("init: mount(/dev/pts) failed"); return 1; }

    char *login_argv[] = { "/usr/bin/login", NULL }; // 
    char *login_envp[] = { NULL };

    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child: become login
            execve("/usr/bin/login", login_argv, login_envp);
            perror("init: execve() failed");
            return 1;
        } else if (pid > 0) {
            // Parent: wait for login to exit then restart it
            wait(NULL);
        } else { perror("init: fork() failed"); return 1; }
    }

    /* >be me
       >make an os
       >i make loop
       >program exits from loop when its not meant to
       >wtf.jpg */
    return 1;
}
