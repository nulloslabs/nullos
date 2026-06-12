#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pw_fd = -1;
static struct passwd pw_static;
static char pw_buf[1024];

static char *field(char **p) {
    char *s = *p;
    while (**p && **p != ':' && **p != '\n') (*p)++;
    if (**p) *(*p)++ = '\0';
    return s;
}

static int read_line(int fd, char *buf, size_t size) {
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) break;
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return n ? 1 : 0;
}

static int parse_passwd(char *line, struct passwd *pwd) {
    char *p = line;
    pwd->pw_name = field(&p);
    pwd->pw_passwd = field(&p);
    pwd->pw_uid = (uid_t)strtoul(field(&p), NULL, 10);
    pwd->pw_gid = (gid_t)strtoul(field(&p), NULL, 10);
    pwd->pw_gecos = field(&p);
    pwd->pw_dir = field(&p);
    pwd->pw_shell = field(&p);
    return pwd->pw_name && *pwd->pw_name;
}

static int lookup(int by_name, const char *name, uid_t uid,
                  struct passwd *pwd, char *buf, size_t buflen,
                  struct passwd **result) {
    int fd = open("/etc/passwd", O_RDONLY);
    if (result) *result = NULL;
    if (fd < 0) return errno;

    while (read_line(fd, buf, buflen)) {
        if (!parse_passwd(buf, pwd)) continue;
        if ((by_name && strcmp(pwd->pw_name, name) == 0) ||
            (!by_name && pwd->pw_uid == uid)) {
            close(fd);
            if (result) *result = pwd;
            return 0;
        }
    }
    close(fd);
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) { if (!name || !pwd || !buf || !result) return EINVAL; return lookup(1, name, 0, pwd, buf, buflen, result); }

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) { if (!pwd || !buf || !result) return EINVAL; return lookup(0, NULL, uid, pwd, buf, buflen, result); }

struct passwd *getpwnam(const char *name) {
    struct passwd *res;
    if (getpwnam_r(name, &pw_static, pw_buf, sizeof(pw_buf), &res)) return NULL;
    return res;
}

struct passwd *getpwuid(uid_t uid) {
    struct passwd *res;
    if (getpwuid_r(uid, &pw_static, pw_buf, sizeof(pw_buf), &res)) return NULL;
    return res;
}

void setpwent(void) { if (pw_fd >= 0) close(pw_fd); pw_fd = open("/etc/passwd", O_RDONLY); }

void endpwent(void) { if (pw_fd >= 0) close(pw_fd); pw_fd = -1; }

struct passwd *getpwent(void) {
    if (pw_fd < 0) setpwent();
    if (pw_fd < 0) return NULL;
    while (read_line(pw_fd, pw_buf, sizeof(pw_buf))) { if (parse_passwd(pw_buf, &pw_static)) return &pw_static; }
    return NULL;
}