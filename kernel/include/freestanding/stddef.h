#pragma once

#define NULL ((void *)0)
#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#if defined(__x86_64__)
typedef unsigned long size_t;
typedef long long off_t;
typedef long ptrdiff_t;
#else
#error "Unsupported architecture for kernel/freestanding/stddef.h."
#endif

typedef int pid_t;
typedef unsigned int wchar_t;
typedef unsigned int wint_t;
