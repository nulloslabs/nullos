#pragma once

#include <main/string.h>

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char processor[65];
    char hardware_platform[65];
    char operating_system[65];
} utsname_t;

extern utsname_t uname_info;

void cache_uname(void);