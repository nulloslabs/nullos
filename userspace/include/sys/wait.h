#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG 1

pid_t wait(int *wstatus);

#ifdef __cplusplus
}
#endif
