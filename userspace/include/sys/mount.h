#pragma once

#define MS_FORCE 0x01
#define MS_DETACH 0x02
#define MS_EXPIRE 0x04

#define UMOUNT_NORMAL 0x00
#define UMOUNT_FORCE 0x01
#define UMOUNT_DETACH 0x02
#define UMOUNT_EXPIRE 0x04
#define UMOUNT_NOFOLLOW 0x08

int mount(const char *source, const char *target, const char *filesystemtype, unsigned long mountflags, const void *data);
int umount(const char *target, int flags);