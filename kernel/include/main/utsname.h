#pragma once

#include <sys/utsname.h>
#include <main/string.h>

extern struct utsname utsname_info;

void cache_utsname(void);
