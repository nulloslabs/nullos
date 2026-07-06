#pragma once

#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>
#include <freestanding/time.h>

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

#define S_IRWXU   (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG   (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO   (S_IROTH | S_IWOTH | S_IXOTH)
#define S_IRUGO   (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO   (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO   (S_IXUSR | S_IXGRP | S_IXOTH)
#define S_IRWXUGO (S_IRWXU | S_IRWXG | S_IRWXO)

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat {
    dev_t     st_dev;     // ID of device containing file
    ino_t     st_ino;     // Inode number
    nlink_t   st_nlink;   // Number of hard links
    mode_t    st_mode;    // Protection
    uid_t     st_uid;     // User ID of owner
    gid_t     st_gid;     // Group ID of owner
    int       __pad0;
    dev_t     st_rdev;    // Device ID (if special file)
    int64_t   st_size;    // Total size, in bytes
    blksize_t st_blksize; // Blocksize for filesystem I/O
    blkcnt_t  st_blocks;  // Number of 512B blocks allocated
    union { time_t st_atime; struct timespec st_atim; };
    union { time_t st_mtime; struct timespec st_mtim; };
    union { time_t st_ctime; struct timespec st_ctim; };
    int64_t   __reserved[3];
};
