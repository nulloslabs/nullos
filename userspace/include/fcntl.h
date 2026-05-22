#pragma once

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0003
#define O_TRUNC 0x0004
#define O_APPEND 0x0005

#define AT_FDCWD 0x0001
#define AT_SYMLINK_NOFOLLOW 0x0002
#define AT_REMOVEDIR 0x0003

int open(const char *path, int flags, ...);
