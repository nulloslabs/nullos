#pragma once

#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>
#include <freestanding/time.h>

struct stat {
    dev_t     st_dev;     /* ID of device containing file */
    ino_t     st_ino;     /* Inode number */
    nlink_t   st_nlink;   /* Number of hard links */
    mode_t    st_mode;    /* Protection */
    uid_t     st_uid;     /* User ID of owner */
    gid_t     st_gid;     /* Group ID of owner */
    int       __pad0;
    dev_t     st_rdev;    /* Device ID (if special file) */
    int64_t   st_size;    /* Total size, in bytes */
    blksize_t st_blksize; /* Blocksize for filesystem I/O */
    blkcnt_t  st_blocks;  /* Number of 512B blocks allocated */
    union { time_t st_atime; struct timespec st_atim; };
    union { time_t st_mtime; struct timespec st_mtim; };
    union { time_t st_ctime; struct timespec st_ctim; };
    int64_t   __reserved[3];
};
