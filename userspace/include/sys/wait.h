#pragma once

#include <stddef.h>

#define WNOHANG 1

pid_t wait(int *wstatus);
