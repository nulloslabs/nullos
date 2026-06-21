#pragma once

#include <freestanding/sys/utsname.h>
#include <main/string.h>

extern struct utsname uname_info;

void cache_uname(void);
