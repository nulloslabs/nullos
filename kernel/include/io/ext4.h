#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <main/spinlocks.h>

/* Defines */

#define EXT4_SUPER_OFFSET 1024ULL
#define EXT4_SUPER_SIZE 1024U
#define EXT4_SUPER_MAGIC 0xEF53U
#define EXT4_EXTENT_MAGIC 0xF30AU
#define EXT4_ROOT_INO 2U
#define EXT4_MAX_MOUNTS 4
#define EXT4_MAX_PATH 768
#define EXT4_MAX_SYMLINKS 40
#define EXT4_MIN_BLOCK_SIZE 1024U
#define EXT4_MAX_BLOCK_SIZE 65536U
#define EXT4_GOOD_OLD_INODE_SIZE 128U
#define EXT4_N_BLOCKS 15U
#define EXT4_EXTENTS_FL 0x00080000U
#define EXT4_HUGE_FILE_FL 0x00040000U
#define EXT4_COMPR_FL 0x00000004U
#define EXT4_IMMUTABLE_FL 0x00000010U
#define EXT4_APPEND_FL 0x00000020U
#define EXT4_NODUMP_FL 0x00000040U
#define EXT4_ENCRYPT_FL 0x00000800U
#define EXT4_VERITY_FL 0x00100000U

#define EXT4_FT_UNKNOWN 0U
#define EXT4_FT_REG_FILE 1U
#define EXT4_FT_DIR 2U
#define EXT4_FT_CHRDEV 3U
#define EXT4_FT_BLKDEV 4U
#define EXT4_FT_FIFO 5U
#define EXT4_FT_SOCK 6U
#define EXT4_FT_SYMLINK 7U

#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004U

#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x00000001U
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x00000002U
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x00000004U
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x00000008U
#define EXT4_FEATURE_INCOMPAT_META_BG 0x00000010U
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x00000040U
#define EXT4_FEATURE_INCOMPAT_64BIT 0x00000080U
#define EXT4_FEATURE_INCOMPAT_MMP 0x00000100U
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x00000200U
#define EXT4_FEATURE_INCOMPAT_EA_INODE 0x00000400U
#define EXT4_FEATURE_INCOMPAT_DIRDATA 0x00001000U
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x00002000U
#define EXT4_FEATURE_INCOMPAT_LARGEDIR 0x00004000U
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x00008000U
#define EXT4_FEATURE_INCOMPAT_ENCRYPT 0x00010000U
#define EXT4_FEATURE_INCOMPAT_CASEFOLD 0x00020000U

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x00000001U
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE 0x00000002U
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR 0x00000004U
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE 0x00000008U
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM 0x00000010U
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK 0x00000020U
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x00000040U
#define EXT4_FEATURE_RO_COMPAT_QUOTA 0x00000100U
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC 0x00000200U
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x00000400U
#define EXT4_FEATURE_RO_COMPAT_READONLY 0x00001000U
#define EXT4_FEATURE_RO_COMPAT_PROJECT 0x00002000U
#define EXT4_FEATURE_RO_COMPAT_VERITY 0x00008000U
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x00010000U

#define EXT4_SUPPORTED_INCOMPAT (EXT4_FEATURE_INCOMPAT_FILETYPE |                                  \
                                 EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |     \
                                 EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED | \
                                 EXT4_FEATURE_INCOMPAT_LARGEDIR)

#define EXT4_SUPPORTED_RO_COMPAT (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |                                   \
                                  EXT4_FEATURE_RO_COMPAT_LARGE_FILE | EXT4_FEATURE_RO_COMPAT_BTREE_DIR |  \
                                  EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM |    \
                                  EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | \
                                  EXT4_FEATURE_RO_COMPAT_QUOTA | EXT4_FEATURE_RO_COMPAT_METADATA_CSUM |   \
                                  EXT4_FEATURE_RO_COMPAT_READONLY | EXT4_FEATURE_RO_COMPAT_PROJECT)

/* Structs and types */

typedef struct {
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blocks_512;
    uint32_t flags;
    uint32_t generation;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    struct timespec btime;
    bool has_btime;
    uint16_t links;
    uint8_t block[60];
} ext4_inode_t;

typedef struct {
    bool active;
    char device[65];
    char target[64];
    uint64_t device_size;
    uint64_t blocks_count;
    uint32_t inodes_count;
    uint32_t first_data_block;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint16_t inode_size;
    uint16_t desc_size;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint32_t groups_count;
    uint64_t gdt_offset;
} ext4_mount_t;

typedef int (*ext4_dir_callback_t)(uint32_t ino, uint8_t type, const char *name, uint8_t name_len, void *context);

typedef struct {
    const char *name;
    size_t length;
    uint32_t ino;
    uint8_t type;
} ext4_lookup_context_t;

typedef struct {
    int wanted;
    int seen;
    char *name;
    size_t name_size;
    uint8_t *type;
    ino_t *ino;
} ext4_readdir_context_t;

struct statx;

/* Extern variables */

extern ext4_mount_t ext4_mounts[EXT4_MAX_MOUNTS];
extern spinlock_t ext4_lock;

/* Functions */

int mount_ext4(const char *source, const char *target);
int unmount_ext4(const char *target);
bool check_ext4_path(const char *path);
int stat_ext4(const char *path, struct stat *st, bool follow);
int statx_ext4_metadata(const char *path, struct statx *stx, bool follow);
int64_t read_ext4(const char *path, void *buffer, uint64_t count, uint64_t offset);
int read_ext4_link(const char *path, char *buffer, size_t size);
int get_next_ext4_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino);
