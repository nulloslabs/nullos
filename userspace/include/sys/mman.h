#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FIXED     0x10

#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int mprotect(void *addr, size_t length, int prot);
int munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif
