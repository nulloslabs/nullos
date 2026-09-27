#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statx.h>
#include <sys/types.h>
#include <time.h>
#include <main/spinlocks.h>

#define EXT4_SUPER_OFFSET        1024ULL
#define EXT4_SUPER_SIZE          1024U
#define EXT4_SUPER_MAGIC         0xEF53U
#define EXT4_EXTENT_MAGIC        0xF30AU
#define EXT4_ROOT_INO            2U
#define EXT4_MAX_MOUNTS          4
#define EXT4_MAX_PATH            768
#define EXT4_MAX_SYMLINKS        40
#define EXT4_MIN_BLOCK_SIZE      1024U
#define EXT4_MAX_BLOCK_SIZE      65536U
#define EXT4_GOOD_OLD_INODE_SIZE 128U
#define EXT4_N_BLOCKS            15U
#define EXT4_ROOT_EXTENT_ENTRIES 4U
#define EXT4_MAX_WRITTEN_EXTENT  32768U
#define EXT4_MAX_LOGICAL_BLOCK   0xFFFFFFFEULL

#define EXT4_CRC32C_POLY      0x82F63B78U
#define EXT4_CRC16_POLY       0xA001U
#define EXT4_CSUM_TYPE_CRC32C 0x01U
#define EXT4_OS_LINUX         0U

#define EXT4_STATE_CLEAN      0x0001U
#define EXT4_STATE_ERRORS     0x0002U
#define EXT4_STATE_ORPHANS    0x0004U
#define EXT4_BG_INODE_UNINIT  0x0001U
#define EXT4_BG_BLOCK_UNINIT  0x0002U

#define EXT4_EXTENTS_FL          0x00080000U
#define EXT4_HUGE_FILE_FL        0x00040000U
#define EXT4_COMPR_FL            0x00000004U
#define EXT4_IMMUTABLE_FL        0x00000010U
#define EXT4_APPEND_FL           0x00000020U
#define EXT4_NODUMP_FL           0x00000040U
#define EXT4_INDEX_FL            0x00001000U
#define EXT4_JOURNAL_DATA_FL     0x00004000U
#define EXT4_ENCRYPT_FL          0x00000800U
#define EXT4_SNAPFILE_FL         0x01000000U
#define EXT4_SNAPFILE_DELETED_FL 0x04000000U
#define EXT4_SNAPFILE_SHRUNK_FL  0x08000000U
#define EXT4_INLINE_DATA_FL      0x10000000U
#define EXT4_VERITY_FL           0x00100000U

#define EXT4_FT_UNKNOWN  0U
#define EXT4_FT_REG_FILE 1U
#define EXT4_FT_DIR      2U
#define EXT4_FT_CHRDEV   3U
#define EXT4_FT_BLKDEV   4U
#define EXT4_FT_FIFO     5U
#define EXT4_FT_SOCK     6U
#define EXT4_FT_SYMLINK  7U

#define EXT4_FEATURE_COMPAT_HAS_JOURNAL    0x0004U
#define EXT4_FEATURE_COMPAT_EXT_ATTR       0x0008U
#define EXT4_FEATURE_COMPAT_RESIZE_INODE   0x0010U
#define EXT4_FEATURE_COMPAT_DIR_INDEX      0x0020U
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200U
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE    0x1000U

#define EXT4_FEATURE_INCOMPAT_COMPRESSION  0x00000001U
#define EXT4_FEATURE_INCOMPAT_FILETYPE    0x00000002U
#define EXT4_FEATURE_INCOMPAT_RECOVER     0x00000004U
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x00000008U
#define EXT4_FEATURE_INCOMPAT_META_BG     0x00000010U
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x00000040U
#define EXT4_FEATURE_INCOMPAT_64BIT       0x00000080U
#define EXT4_FEATURE_INCOMPAT_MMP         0x00000100U
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x00000200U
#define EXT4_FEATURE_INCOMPAT_EA_INODE    0x00000400U
#define EXT4_FEATURE_INCOMPAT_DIRDATA     0x00001000U
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x00002000U
#define EXT4_FEATURE_INCOMPAT_LARGEDIR    0x00004000U
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x00008000U
#define EXT4_FEATURE_INCOMPAT_ENCRYPT     0x00010000U
#define EXT4_FEATURE_INCOMPAT_CASEFOLD    0x00020000U

#define EXT4_SUPPORTED_COMPAT (EXT4_FEATURE_COMPAT_HAS_JOURNAL | EXT4_FEATURE_COMPAT_EXT_ATTR | EXT4_FEATURE_COMPAT_RESIZE_INODE | EXT4_FEATURE_COMPAT_DIR_INDEX | EXT4_FEATURE_COMPAT_SPARSE_SUPER2 | EXT4_FEATURE_COMPAT_ORPHAN_FILE)

#define EXT4_JOURNAL_MAGIC            0xC03B3998U
#define EXT4_JOURNAL_DESCRIPTOR       1U
#define EXT4_JOURNAL_COMMIT           2U
#define EXT4_JOURNAL_SUPER_V1         3U
#define EXT4_JOURNAL_SUPER_V2         4U
#define EXT4_JOURNAL_REVOKE           5U

#define EXT4_JOURNAL_TAG_ESCAPE       1U
#define EXT4_JOURNAL_TAG_SAME_UUID    2U
#define EXT4_JOURNAL_TAG_DELETED      4U
#define EXT4_JOURNAL_TAG_LAST         8U

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER   0x00000001U
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE     0x00000002U
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR      0x00000004U
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE      0x00000008U
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM       0x00000010U
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK      0x00000020U
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE    0x00000040U
#define EXT4_FEATURE_RO_COMPAT_QUOTA          0x00000100U
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC       0x00000200U
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM  0x00000400U
#define EXT4_FEATURE_RO_COMPAT_READONLY       0x00001000U
#define EXT4_FEATURE_RO_COMPAT_PROJECT        0x00002000U
#define EXT4_FEATURE_RO_COMPAT_VERITY         0x00008000U
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x00010000U

#define EXT4_SUPPORTED_INCOMPAT (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED | EXT4_FEATURE_INCOMPAT_LARGEDIR)

#define EXT4_SUPPORTED_RO_COMPAT (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE | EXT4_FEATURE_RO_COMPAT_BTREE_DIR | EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM | EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | EXT4_FEATURE_RO_COMPAT_QUOTA | EXT4_FEATURE_RO_COMPAT_METADATA_CSUM | EXT4_FEATURE_RO_COMPAT_READONLY | EXT4_FEATURE_RO_COMPAT_PROJECT)

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
    uint32_t deleted;
    struct timespec btime;
    bool has_btime;
    uint16_t links;
    uint16_t extra_size;
    uint8_t block[60];
} ext4_inode_t;

typedef struct {
    bool active;
    bool read_only;
    char device[65];
    char target[64];
    uint64_t device_size;
    uint64_t blocks_count;
    uint64_t free_blocks;
    uint64_t reserved_blocks;
    uint32_t inodes_count;
    uint32_t first_data_block;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_blocks_per_group;
    uint16_t inode_size;
    uint16_t desc_size;
    uint16_t state;
    uint16_t reserved_uid;
    uint16_t reserved_gid;
    uint16_t reserved_gdt_blocks;
    uint8_t checksum_type;
    uint8_t uuid[16];
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint32_t checksum_seed;
    uint32_t groups_count;
    uint64_t gdt_offset;
    bool has_journal;
    uint32_t journal_ino;
    uint32_t journal_maxlen;
    uint32_t journal_first;
    uint32_t journal_sequence;
    uint32_t journal_head;
    uint8_t journal_uuid[16];
} ext4_mount_t;

typedef struct {
    uint64_t byte_offset;
    uint32_t group;
    uint32_t block_count;
} ext4_inode_location_t;

typedef struct {
    uint8_t descriptor[64];
    uint8_t *bitmap;
    uint64_t bitmap_block;
    uint32_t group;
    uint32_t free_blocks;
} ext4_group_state_t;

typedef struct {
    uint64_t physical;
    bool allocated;
    bool initialize;
} ext4_block_state_t;

typedef struct {
    ext4_group_state_t *groups;
    ext4_block_state_t *blocks;
    uint8_t extents[EXT4_N_BLOCKS * 4U];
    size_t group_count;
    size_t group_capacity;
    size_t block_count;
    uint32_t first_block;
    uint32_t allocated_blocks;
} ext4_write_context_t;

typedef struct {
    uint64_t fs_block;
    uint8_t *data;
} ext4_journal_record_t;

typedef struct {
    ext4_journal_record_t *records;
    size_t count;
    size_t capacity;
} ext4_journal_batch_t;

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

typedef int (*ext4_dir_callback_t)(uint32_t ino, uint8_t type, const char *name, uint8_t name_len, void *context);

extern ext4_mount_t ext4_mounts[EXT4_MAX_MOUNTS];
extern spinlock_t ext4_lock;

int mount_ext4(const char *source, const char *path, unsigned long flags, const char *data);
int unmount_ext4(const char *path);
bool check_ext4_path(const char *path);
int stat_ext4(const char *path, struct stat *st, bool follow);
int statx_ext4_metadata(const char *path, struct statx *stx, bool follow);
int create_ext4(const char *path, mode_t mode, uid_t uid, gid_t gid);
int symlink_ext4(const char *target, const char *path, uid_t uid, gid_t gid);
int link_ext4(const char *oldpath, const char *newpath);
int64_t read_ext4(const char *path, void *buffer, uint64_t count, uint64_t offset);
int64_t write_ext4(const char *path, const void *buffer, uint64_t count, uint64_t offset, bool append);
int truncate_ext4(const char *path, uint64_t length);
int unlink_ext4(const char *path);
int set_ext4_times(const char *path, struct timespec atime, bool set_atime, struct timespec mtime, bool set_mtime, bool follow);
int chmod_ext4(const char *path, mode_t mode, bool follow);
int chown_ext4(const char *path, uid_t uid, gid_t gid, bool follow);
int mkdir_ext4(const char *path, mode_t mode, uid_t uid, gid_t gid);
int rmdir_ext4(const char *path);
int rename_ext4(const char *oldpath, const char *newpath);
int sync_ext4(const char *path);
bool check_ext4_writable(const char *path);
int read_ext4_link(const char *path, char *buffer, size_t size);
int get_next_ext4_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino);
