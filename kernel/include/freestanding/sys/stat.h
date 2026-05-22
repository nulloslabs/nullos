#pragma once

#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>

struct stat {
    dev_t     st_dev;     /* ID of device containing file */
    ino_t     st_ino;     /* Inode number */
    mode_t    st_mode;    /* Protection */
    nlink_t   st_nlink;   /* Number of hard links */
    uid_t     st_uid;     /* User ID of owner */
    gid_t     st_gid;     /* Group ID of owner */
    dev_t     st_rdev;    /* Device ID (if special file) */
    int64_t   st_size;    /* Total size, in bytes */
    blksize_t st_blksize; /* Blocksize for filesystem I/O */
    blkcnt_t  st_blocks;  /* Number of 512B blocks allocated */
    time_t    st_atime;   /* Time of last access */
    time_t    st_mtime;   /* Time of last modification */
    time_t    st_ctime;   /* Time of last status change */
};
