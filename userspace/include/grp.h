#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result);
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result);
void endgrent(void);
struct group *getgrent(void);
void setgrent(void);

#ifdef __cplusplus
}
#endif