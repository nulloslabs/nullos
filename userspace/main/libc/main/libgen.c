#include <libgen.h>
#include <string.h>

char *basename(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    char *end;

    if (!path || !*path) return dot;

    end = path + strlen(path) - 1;
    while (end > path && *end == '/') *end-- = '\0';
    if (end == path && *end == '/') return slash;

    char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

char *dirname(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    char *end;

    if (!path || !*path) return dot;

    end = path + strlen(path) - 1;
    while (end > path && *end == '/') *end-- = '\0';
    if (end == path && *end == '/') return slash;

    char *last = strrchr(path, '/');
    if (!last) return dot;
    while (last > path && *last == '/') last--;
    if (last == path && *last == '/') {
        path[1] = '\0';
        return path;
    }
    last[1] = '\0';
    return path;
}
