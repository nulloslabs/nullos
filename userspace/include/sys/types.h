#pragma once

#if defined(__i386__)
typedef unsigned long long dev_t;
typedef unsigned int nlink_t;
typedef unsigned long long ino_t;
typedef long long blkcnt_t;
#elif defined(__x86_64__)
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef unsigned long ino_t;
typedef long blkcnt_t;
#else
#error "Unsupported architecture for sys/types.h."
#endif

typedef long time_t;
typedef int id_t;
typedef int key_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef long blksize_t;
typedef long clock_t;
