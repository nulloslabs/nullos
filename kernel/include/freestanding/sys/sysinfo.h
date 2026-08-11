#pragma once

#include <stdint.h>

#define SI_LOAD_SHIFT 16

struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    uint16_t procs;
    uint16_t __pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    uint32_t mem_unit;
    uint32_t __pad2;
};
