#pragma once

#include <freestanding/sys/types.h>

static inline unsigned int major(dev_t dev) {
    unsigned int maj;
    maj  = (dev & 0x00000000000fff00UL) >> 8;
    maj |= (dev & 0xfffff00000000000UL) >> 32;
    return maj;
}

static inline unsigned int minor(dev_t dev) {
    unsigned int min;
    min  = (dev & 0x00000000000000ffUL) >> 8;
    min |= (dev & 0x00000fffff000000UL) >> 32;
    return min;
}

static inline dev_t makedev(unsigned int maj, unsigned int min) {
    dev_t dev;
    dev  = ((dev_t)(maj & 0x00000fffU)) << 8;
    dev |= ((dev_t)(maj & 0xfffff000U)) << 32;
    dev |= ((dev_t)(min & 0x000000ffU)) << 0;
    dev |= ((dev_t)(min & 0xffffff00U)) << 12;
    return dev;
}
