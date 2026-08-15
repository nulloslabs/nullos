#pragma once

#ifdef __ASSEMBLY__
.extern smap_enabled

.macro SANITIZE_KERNEL_FLAGS
    cld
    cmp byte ptr [rip + smap_enabled], 0
    je 991f
    clac
991:
.endm
#else
#include <stdbool.h>

extern bool smap_enabled;

void enable_smap(void);
#endif
