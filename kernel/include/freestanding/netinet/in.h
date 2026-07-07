#pragma once

#include <freestanding/stdint.h>

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) { return ((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8) | ((x & 0x0000FF00) << 8)  | ((x & 0x000000FF) << 24); }
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }
