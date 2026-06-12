#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int gr_fd = -1;
static struct group gr_static;
static char gr_buf[1024];
static char *gr_members[64];

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

static char *field(char **p) {
    char *s = *p;
    while (**p && **p != ':' && **p != '\n') (*p)++;
    if (**p) *(*p)++ = '\0';
    return s;
}

static void split_members(char *s, char **members, size_t max) {
    size_t n = 0;
    while (*s && n + 1 < max) {
        members[n++] = s;
        while (*s && *s != ',' && *s != '\n') s++;
        if (*s) *s++ = '\0';
    }
    members[n] = NULL;
}

static int parse_group(char *line, struct group *grp, char **members, size_t max) {
    char *p = line;
    grp->gr_name = field(&p);
    grp->gr_passwd = field(&p);
    grp->gr_gid = (gid_t)strtoul(field(&p), NULL, 10);
    split_members(field(&p), members, max);
    grp->gr_mem = members;
    return grp->gr_name && *grp->gr_name;
}

static int lookup(int by_name, const char *name, gid_t gid,
                  struct group *grp, char *buf, size_t buflen,
                  struct group **result) {
    int fd = open("/etc/group", O_RDONLY);
    if (result) *result = NULL;
    if (fd < 0) return errno;

    while (read_line(fd, buf, buflen)) {
        if (!parse_group(buf, grp, gr_members, 64)) continue;
        if ((by_name && strcmp(grp->gr_name, name) == 0) ||
            (!by_name && grp->gr_gid == gid)) {
            close(fd);
            if (result) *result = grp;
            return 0;
        }
    }
    close(fd);
    return 0;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result) { if (!name || !grp || !buf || !result) return EINVAL; return lookup(1, name, 0, grp, buf, buflen, result); }

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result) { if (!grp || !buf || !result) return EINVAL; return lookup(0, NULL, gid, grp, buf, buflen, result); }

struct group *getgrnam(const char *name) {
    struct group *res;
    if (getgrnam_r(name, &gr_static, gr_buf, sizeof(gr_buf), &res)) return NULL;
    return res;
}

struct group *getgrgid(gid_t gid) {
    struct group *res;
    if (getgrgid_r(gid, &gr_static, gr_buf, sizeof(gr_buf), &res)) return NULL;
    return res;
}

void setgrent(void) { if (gr_fd >= 0) close(gr_fd); gr_fd = open("/etc/group", O_RDONLY); }

void endgrent(void) { if (gr_fd >= 0) close(gr_fd); gr_fd = -1; }

struct group *getgrent(void) {
    if (gr_fd < 0) setgrent();
    if (gr_fd < 0) return NULL;
    while (read_line(gr_fd, gr_buf, sizeof(gr_buf))) { if (parse_group(gr_buf, &gr_static, gr_members, 64)) return &gr_static; }
    return NULL;
}