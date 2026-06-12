#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <sys/reboot.h>
#include <stdlib.h>
#include <signal.h>

#define SH_LINE_MAX 256
#define SH_ARG_MAX 32

static int read_line(char *line, int size) {
    int n = read(0, line, size - 1);
    if (n <= 0) return n;

    if (line[n - 1] == '\n') {
        line[n - 1] = '\0';
    } else { line[n] = '\0'; }

    return n;
}

static int parse_args(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = p;

        char *out = p;
        char quote = '\0';
        while (*p) {
            if (quote) {
                if (*p == quote) {
                    p++;
                    quote = '\0';
                    continue;
                }
                *out++ = *p++;
                continue;
            }

            if (*p == '\'' || *p == '"') { quote = *p++; continue; }

            if (*p == ' ' || *p == '\t') break;
            *out++ = *p++;
        }

        if (*p) p++;
        *out = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

static int has_slash(const char *s) { return strchr(s, '/') != NULL; }

static void build_prog_path(const char *cmd, char *out, int out_size) {
    if (has_slash(cmd)) {
        strncpy(out, cmd, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    strncpy(out, "/usr/bin/", out_size - 1);
    out[out_size - 1] = '\0';
    strncat(out, cmd, out_size - strlen(out) - 1);
}

static void print_usage(void) {
    printf("Usage for sh:\n");
    printf("  sh [OPTIONS]\n\n");
    printf("Options:\n");
    printf("  --help: Show help dialouge.\n");
    printf("  -c <command>: Execute the command string that follows.\n");
}

static int run_command(char *line, char **envp, int *exit_shell) {
    char path[SH_LINE_MAX];
    char *cmd_argv[SH_ARG_MAX];

    int cmd_argc = parse_args(line, cmd_argv, SH_ARG_MAX);
    if (cmd_argc == 0) return 0;

    // Built-in commands are kewl :3

    if (strcmp(cmd_argv[0], "exit") == 0) { if (exit_shell) *exit_shell = 1; return 0; }

    if (strcmp(cmd_argv[0], "cd") == 0) {
        const char *dir = (cmd_argc > 1) ? cmd_argv[1] : "/";
        if (chdir(dir) < 0) { perror("cd"); return 1; }
        return 0;
    }

    if (strcmp(cmd_argv[0], "pwd") == 0) {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) != NULL) { printf("%s\n", cwd); return 0; }

        perror("pwd");
        return 1;
    }

    build_prog_path(cmd_argv[0], path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("%s: %s\n", cmd_argv[0], strerror(errno)); return 127; }
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        execve(path, cmd_argv, envp);
        perror(cmd_argv[0]);
        exit(127);
    }

    if (pid < 0) { perror("fork"); return 1; }

    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }

    return status;
}

int main(int argc, char **argv, char **envp) {
    char line[SH_LINE_MAX];

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0) { print_usage(); return 0; }

        if (strcmp(argv[1], "-c") == 0) {
            if (argc < 3) { fprintf(stderr, "sh: '-c' requires an argument\n"); return 2; }

            strncpy(line, argv[2], sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            return run_command(line, envp, NULL);
        }

        fprintf(stderr, "sh: unknown argument: %s\n", argv[1]);
        return 2;
    }

    for (;;) {
        printf("$ ");
        fflush(stdout);

        int n = read_line(line, sizeof(line));
        if (n <= 0) { printf("\n"); return 0; }

        int exit_shell = 0;
        run_command(line, envp, &exit_shell);
        if (exit_shell) { return 0; }
    }
}