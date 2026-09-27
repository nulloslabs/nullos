#pragma once

#include <util/string.h>

#define UTSNAME_LENGTH 65

struct utsname {
    char sysname[UTSNAME_LENGTH];
    char nodename[UTSNAME_LENGTH];
    char release[UTSNAME_LENGTH];
    char version[UTSNAME_LENGTH];
    char machine[UTSNAME_LENGTH];
    char domainname[UTSNAME_LENGTH];
};

extern struct utsname utsname;

void cache_utsname(void);
