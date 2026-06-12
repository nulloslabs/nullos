#pragma once

#include <sys/types.h>

#define major(dev) ((unsigned int)(((dev) >> 8) & 0xfffU))
#define minor(dev) ((unsigned int)(((dev) & 0xffU) | (((dev) >> 12) & 0xfff00U)))
#define makedev(maj, min) ((dev_t)((((dev_t)(maj) & 0xfffU) << 8) | \
                           ((dev_t)(min) & 0xffU) | \
                           ((((dev_t)(min) & 0xfff00U) << 12))))