#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <main/spinlocks.h>

// Defines

#define ISO9660_SECTOR_SIZE 2048U
#define ISO9660_PVD_SECTOR 16U
#define ISO9660_VD_SECTOR_LAST 32U
#define ISO9660_VD_TYPE_PRIMARY 1U
#define ISO9660_VD_TYPE_SUPPLEMENTARY 2U
#define ISO9660_VD_TYPE_TERMINATOR 255U
#define ISO9660_PVD_BLOCK_SIZE_OFFSET 128U
#define ISO9660_PVD_ROOT_OFFSET 156U
#define ISO9660_SVD_ESCAPE_OFFSET 88U
#define ISO9660_RECORD_XAR_OFFSET 1U
#define ISO9660_RECORD_LBA_OFFSET 2U
#define ISO9660_RECORD_SIZE_OFFSET 10U
#define ISO9660_RECORD_TIME_OFFSET 18U
#define ISO9660_RECORD_FLAGS_OFFSET 25U
#define ISO9660_RECORD_NAME_LENGTH_OFFSET 32U
#define ISO9660_RECORD_NAME_OFFSET 33U
#define ISO9660_FLAG_HIDDEN 0x01U
#define ISO9660_FLAG_DIR 0x02U
#define ISO9660_ROCK_RIDGE_NM_CONTINUE 0x01U
#define ISO9660_ROCK_RIDGE_NM_CURRENT 0x02U
#define ISO9660_ROCK_RIDGE_NM_PARENT 0x04U
#define ISO9660_ROCK_RIDGE_TF_MODIFY 0x02U
#define ISO9660_ROCK_RIDGE_TF_LONG 0x80U
#define ISO9660_ROCK_RIDGE_SL_CURRENT 0x02U
#define ISO9660_ROCK_RIDGE_SL_PARENT 0x04U
#define ISO9660_ROCK_RIDGE_SL_ROOT 0x08U
#define ISO9660_ROCK_RIDGE_CE_DEPTH_MAX 4
#define ISO9660_ROCK_RIDGE_CE_BYTES_MAX 4096U
#define ISO9660_ROCK_RIDGE_LINK_DEPTH_MAX 8
#define ISO9660_MAX_MOUNTS 4
#define ISO9660_DEVICE_MAX 65
#define ISO9660_TARGET_MAX 64
#define ISO9660_NAME_MAX 256
#define ISO9660_RELATIVE_MAX 256
#define ISO9660_DIR_MAX_BYTES (1024U * 1024U)
#define ISO9660_MAX_COMPONENT 255

// Structs and types

typedef struct {
    bool active;
    char device[ISO9660_DEVICE_MAX];
    char target[ISO9660_TARGET_MAX];
    uint64_t device_size;
    uint32_t sector_size;
    uint32_t root_lba;
    uint32_t root_size;
    bool has_rock_ridge;
    bool has_joliet;
    uint32_t joliet_root_lba;
    uint32_t joliet_root_size;
} iso9660_mount_t;

typedef struct {
    uint32_t record_lba;
    uint32_t record_size;
    uint32_t record_xar;
    uint8_t record_flags;
    int64_t record_time;
    uint32_t rock_ridge_mode;
    uint32_t rock_ridge_nlink;
    uint32_t rock_ridge_uid;
    uint32_t rock_ridge_gid;
    uint32_t rock_ridge_ino;
    bool rock_ridge_symlink;
    bool rock_ridge_named;
    char link_target[ISO9660_NAME_MAX];
    char name[ISO9660_NAME_MAX];
    size_t name_len;
} iso9660_record_t;

typedef int (*iso9660_dir_callback_t)(const iso9660_record_t *record, void *context);

typedef struct {
    const char *component;
    size_t component_len;
    bool matched;
    iso9660_record_t found;
} iso9660_lookup_context_t;

typedef struct {
    int wanted;
    int seen;
    char *name;
    size_t name_size;
    uint8_t *type;
    ino_t *ino;
} iso9660_readdir_context_t;

// Extern variables

extern iso9660_mount_t iso9660_mounts[ISO9660_MAX_MOUNTS];
extern spinlock_t iso9660_lock;

// Functions

int mount_iso9660(const char *source, const char *path, unsigned long flags, const char *data);
int unmount_iso9660(const char *path);
bool check_iso9660_path(const char *path);
bool get_iso9660_mount_root(const char *path, char *root, size_t root_size);
int stat_iso9660(const char *path, struct stat *st, bool follow);
int read_iso9660_link(const char *path, char *target, size_t target_size);
int64_t read_iso9660(const char *path, void *buffer, uint64_t count, uint64_t offset);
int get_next_iso9660_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino);
