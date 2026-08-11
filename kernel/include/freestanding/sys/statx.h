#pragma once

#include <stdint.h>

#define STATX_TYPE        0x00000001U
#define STATX_MODE        0x00000002U
#define STATX_NLINK       0x00000004U
#define STATX_UID         0x00000008U
#define STATX_GID         0x00000010U
#define STATX_ATIME       0x00000020U
#define STATX_MTIME       0x00000040U
#define STATX_CTIME       0x00000080U
#define STATX_INO         0x00000100U
#define STATX_SIZE        0x00000200U
#define STATX_BLOCKS      0x00000400U
#define STATX_BASIC_STATS 0x000007ffU
#define STATX_BTIME       0x00000800U
#define STATX_MNT_ID      0x00001000U
#define STATX_DIOALIGN    0x00002000U
#define STATX_MNT_ID_UNIQUE 0x00004000U
#define STATX_SUBVOL      0x00008000U
#define STATX_WRITE_ATOMIC 0x00010000U
#define STATX_DIO_READ_ALIGN 0x00020000U
#define STATX__RESERVED   0x80000000U
#define STATX_ALL         0x00000fffU

#define STATX_ATTR_COMPRESSED   0x00000004ULL
#define STATX_ATTR_IMMUTABLE    0x00000010ULL
#define STATX_ATTR_APPEND       0x00000020ULL
#define STATX_ATTR_NODUMP       0x00000040ULL
#define STATX_ATTR_ENCRYPTED    0x00000800ULL
#define STATX_ATTR_AUTOMOUNT    0x00001000ULL
#define STATX_ATTR_MOUNT_ROOT   0x00002000ULL
#define STATX_ATTR_VERITY       0x00100000ULL
#define STATX_ATTR_DAX          0x00200000ULL
#define STATX_ATTR_WRITE_ATOMIC 0x00400000ULL

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

struct statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __reserved;
};

struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t stx_subvol;
    uint32_t stx_atomic_write_unit_min;
    uint32_t stx_atomic_write_unit_max;
    uint32_t stx_atomic_write_segments_max;
    uint32_t stx_dio_read_offset_align;
    uint32_t stx_atomic_write_unit_max_opt;
    uint32_t __spare2[1];
    uint64_t __spare3[8];
};
