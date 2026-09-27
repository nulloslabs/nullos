#pragma once

#include <freestanding/sys/utsname.h>
#include <util/string.h>

extern struct utsname utsname;

void cache_utsname(void);
