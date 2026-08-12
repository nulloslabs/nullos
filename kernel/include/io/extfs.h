#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <main/spinlocks.h>

/* Defines */

#define EXTFS_SUPER_OFFSET 1024ULL
#define EXTFS_SUPER_SIZE 1024U
#define EXTFS_SUPER_MAGIC 0xEF53U
#define EXTFS_EXTENT_MAGIC 0xF30AU
#define EXTFS_ROOT_INO 2U
#define EXTFS_MAX_MOUNTS 4
#define EXTFS_MAX_PATH 768
#define EXTFS_MAX_SYMLINKS 40
#define EXTFS_MIN_BLOCK_SIZE 1024U
#define EXTFS_MAX_BLOCK_SIZE 65536U
#define EXTFS_GOOD_OLD_INODE_SIZE 128U
#define EXTFS_N_BLOCKS 15U
#define EXTFS_EXTENTS_FL 0x00080000U
#define EXTFS_HUGE_FILE_FL 0x00040000U
#define EXTFS_COMPR_FL 0x00000004U
#define EXTFS_IMMUTABLE_FL 0x00000010U
#define EXTFS_APPEND_FL 0x00000020U
#define EXTFS_NODUMP_FL 0x00000040U
#define EXTFS_ENCRYPT_FL 0x00000800U
#define EXTFS_VERITY_FL 0x00100000U

#define EXTFS_FT_UNKNOWN 0U
#define EXTFS_FT_REG_FILE 1U
#define EXTFS_FT_DIR 2U
#define EXTFS_FT_CHRDEV 3U
#define EXTFS_FT_BLKDEV 4U
#define EXTFS_FT_FIFO 5U
#define EXTFS_FT_SOCK 6U
#define EXTFS_FT_SYMLINK 7U

#define EXTFS_FEATURE_COMPAT_HAS_JOURNAL 0x0004U

#define EXTFS_FEATURE_INCOMPAT_COMPRESSION 0x00000001U
#define EXTFS_FEATURE_INCOMPAT_FILETYPE 0x00000002U
#define EXTFS_FEATURE_INCOMPAT_RECOVER 0x00000004U
#define EXTFS_FEATURE_INCOMPAT_JOURNAL_DEV 0x00000008U
#define EXTFS_FEATURE_INCOMPAT_META_BG 0x00000010U
#define EXTFS_FEATURE_INCOMPAT_EXTENTS 0x00000040U
#define EXTFS_FEATURE_INCOMPAT_64BIT 0x00000080U
#define EXTFS_FEATURE_INCOMPAT_MMP 0x00000100U
#define EXTFS_FEATURE_INCOMPAT_FLEX_BG 0x00000200U
#define EXTFS_FEATURE_INCOMPAT_EA_INODE 0x00000400U
#define EXTFS_FEATURE_INCOMPAT_DIRDATA 0x00001000U
#define EXTFS_FEATURE_INCOMPAT_CSUM_SEED 0x00002000U
#define EXTFS_FEATURE_INCOMPAT_LARGEDIR 0x00004000U
#define EXTFS_FEATURE_INCOMPAT_INLINE_DATA 0x00008000U
#define EXTFS_FEATURE_INCOMPAT_ENCRYPT 0x00010000U
#define EXTFS_FEATURE_INCOMPAT_CASEFOLD 0x00020000U

#define EXTFS_FEATURE_RO_COMPAT_SPARSE_SUPER 0x00000001U
#define EXTFS_FEATURE_RO_COMPAT_LARGE_FILE 0x00000002U
#define EXTFS_FEATURE_RO_COMPAT_BTREE_DIR 0x00000004U
#define EXTFS_FEATURE_RO_COMPAT_HUGE_FILE 0x00000008U
#define EXTFS_FEATURE_RO_COMPAT_GDT_CSUM 0x00000010U
#define EXTFS_FEATURE_RO_COMPAT_DIR_NLINK 0x00000020U
#define EXTFS_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x00000040U
#define EXTFS_FEATURE_RO_COMPAT_QUOTA 0x00000100U
#define EXTFS_FEATURE_RO_COMPAT_BIGALLOC 0x00000200U
#define EXTFS_FEATURE_RO_COMPAT_METADATA_CSUM 0x00000400U
#define EXTFS_FEATURE_RO_COMPAT_READONLY 0x00001000U
#define EXTFS_FEATURE_RO_COMPAT_PROJECT 0x00002000U
#define EXTFS_FEATURE_RO_COMPAT_VERITY 0x00008000U
#define EXTFS_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x00010000U

#define EXTFS_SUPPORTED_INCOMPAT (EXTFS_FEATURE_INCOMPAT_FILETYPE |                                  \
                                 EXTFS_FEATURE_INCOMPAT_EXTENTS | EXTFS_FEATURE_INCOMPAT_64BIT |     \
                                 EXTFS_FEATURE_INCOMPAT_FLEX_BG | EXTFS_FEATURE_INCOMPAT_CSUM_SEED | \
                                 EXTFS_FEATURE_INCOMPAT_LARGEDIR)

#define EXTFS_SUPPORTED_RO_COMPAT (EXTFS_FEATURE_RO_COMPAT_SPARSE_SUPER |                                   \
                                  EXTFS_FEATURE_RO_COMPAT_LARGE_FILE | EXTFS_FEATURE_RO_COMPAT_BTREE_DIR |  \
                                  EXTFS_FEATURE_RO_COMPAT_HUGE_FILE | EXTFS_FEATURE_RO_COMPAT_GDT_CSUM |    \
                                  EXTFS_FEATURE_RO_COMPAT_DIR_NLINK | EXTFS_FEATURE_RO_COMPAT_EXTRA_ISIZE | \
                                  EXTFS_FEATURE_RO_COMPAT_QUOTA | EXTFS_FEATURE_RO_COMPAT_METADATA_CSUM |   \
                                  EXTFS_FEATURE_RO_COMPAT_READONLY | EXTFS_FEATURE_RO_COMPAT_PROJECT)

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
} extfs_inode_t;

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
} extfs_mount_t;

typedef int (*extfs_dir_callback_t)(uint32_t ino, uint8_t type, const char *name, uint8_t name_len, void *context);

typedef struct {
    const char *name;
    size_t length;
    uint32_t ino;
    uint8_t type;
} extfs_lookup_context_t;

typedef struct {
    int wanted;
    int seen;
    char *name;
    size_t name_size;
    uint8_t *type;
    ino_t *ino;
} extfs_readdir_context_t;

struct statx;

/* Extern variables */

extern extfs_mount_t extfs_mounts[EXTFS_MAX_MOUNTS];
extern spinlock_t extfs_lock;

/* Functions */

int mount_extfs(const char *source, const char *target);
int unmount_extfs(const char *target);
bool check_extfs_path(const char *path);
int stat_extfs(const char *path, struct stat *st, bool follow);
int statx_extfs_metadata(const char *path, struct statx *stx, bool follow);
int64_t read_extfs(const char *path, void *buffer, uint64_t count, uint64_t offset);
int read_extfs_link(const char *path, char *buffer, size_t size);
int get_next_extfs_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino);
