#pragma once

#include <sys/utsname.h>
#include <main/string.h>

extern struct utsname utsname;

void cache_utsname(void);
