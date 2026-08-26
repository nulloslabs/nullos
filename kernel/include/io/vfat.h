#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <main/spinlocks.h>

// Boot sector magic values
#define FAT32_SUPER_MAGIC 0xEB5290U

// GPT type GUID for FAT32 (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 in mixed-endian)
extern const uint8_t vfat_gpt_guid[16];

// FAT32 cluster sentinel values
#define FAT32_CHAIN_EOC 0x0FFFFFF8U
#define FAT32_CHAIN_EOF 0x0FFFFFFFU
#define FAT32_CLUSTER_FREE 0U

// Directory entry attribute flags
#define FAT32_ATTR_READ_ONLY 0x01U
#define FAT32_ATTR_HIDDEN 0x02U
#define FAT32_ATTR_SYSTEM 0x04U
#define FAT32_ATTR_VOLUME_ID 0x08U
#define FAT32_ATTR_DIRECTORY 0x10U
#define FAT32_ATTR_ARCHIVE 0x20U
#define FAT32_ATTR_LFN 0x0FU

// Limits
#define VFAT_MAX_MOUNTS 4
#define VFAT_MAX_PATH 768
#define VFAT_MAX_COMPONENT 255
#define VFAT_MAX_NAME 260
#define VFAT_MAX_LFN_ENTRIES 20
#define VFAT_DEVICE_MAX 65
#define VFAT_TARGET_MAX 64
#define VFAT_DIR_MAX_BYTES (1024U * 1024U)

// Boot sector byte offsets
#define FAT32_BS_BYTES_PER_SEC 11
#define FAT32_BS_SEC_PERCLUS 13
#define FAT32_BS_RSVD_SEC_CNT 14
#define FAT32_BS_NUM_FATS 16
#define FAT32_BS_ROOT_ENT_CNT 17
#define FAT32_BS_TOT_SEC_16 19
#define FAT32_BS_MEDIA 21
#define FAT32_BS_FAT_SZ_16 22
#define FAT32_BS_HIDD_SEC 28
#define FAT32_BS_TOT_SEC_32 32
#define FAT32_BS_FAT_SZ_32 36
#define FAT32_BS_EXT_FLAGS 40
#define FAT32_BS_ROOTCLUS 44
#define FAT32_BS_FSI 48
#define FAT32_BS_BKBOOTSEC 50
#define FAT32_BS_BOOT_SIG_32 66
#define FAT32_BS_VOL_ID 67
#define FAT32_BS_VOL_LAB 71
#define FAT32_BS_FILSYSTYPE 82

// Directory entry byte offsets
#define FAT32_DE_NAME 0
#define FAT32_DE_EXT 8
#define FAT32_DE_ATTR 11
#define FAT32_DE_NTRES 12
#define FAT32_DE_CRT_TENTH 13
#define FAT32_DE_CRT_TIME 14
#define FAT32_DE_CRT_DATE 16
#define FAT32_DE_LST_ACC_DATE 18
#define FAT32_DE_FSTCLUS_HI 20
#define FAT32_DE_WRT_TIME 22
#define FAT32_DE_WRT_DATE 24
#define FAT32_DE_FSTCLUS_LO 26
#define FAT32_DE_FILESIZE 28
#define FAT32_DE_SIZE 32

// Long file name entry byte offsets
#define FAT32_LFN_ORDER 0
#define FAT32_LFN_NAME1 1
#define FAT32_LFN_ATTR 11
#define FAT32_LFN_CHECKSUM 13
#define FAT32_LFN_NAME2 14
#define FAT32_LFN_NAME3 28

typedef struct {
    bool active;
    char device[VFAT_DEVICE_MAX];
    char target[VFAT_TARGET_MAX];
    uint64_t device_size;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint64_t partition_offset;
    uint64_t fat_offset;
    uint64_t data_offset;
    uint32_t fsinfo_sector;
    uint32_t volume_id;
    uint8_t fat_bits;
    uint16_t root_dir_entries;
    uint64_t root_dir_offset;
} vfat_mount_t;

typedef struct {
    uint32_t first_cluster;
    uint32_t file_size;
    uint8_t attr;
    uint32_t crt_time;
    uint32_t crt_date;
    uint32_t lst_acc_date;
    uint32_t wrt_time;
    uint32_t wrt_date;
    char long_name[VFAT_MAX_NAME];
    size_t long_name_len;
    char short_name[13];
    size_t short_name_len;
} vfat_dirent_t;

typedef int (*vfat_dir_callback_t)(const vfat_dirent_t *dirent, void *context);

typedef struct {
    const char *component;
    size_t component_len;
    bool matched;
    vfat_dirent_t found;
} vfat_lookup_context_t;

typedef struct {
    int wanted;
    int seen;
    char *name;
    size_t name_size;
    uint8_t *type;
    ino_t *ino;
} vfat_readdir_context_t;

extern vfat_mount_t vfat_mounts[VFAT_MAX_MOUNTS];
extern spinlock_t vfat_lock;

int mount_vfat(const char *source, const char *target, unsigned long flags, const char *data);
int unmount_vfat(const char *target);
bool check_vfat_path(const char *path);
int stat_vfat(const char *path, struct stat *st, bool follow);
int64_t read_vfat(const char *path, void *buffer, uint64_t count, uint64_t offset);
int get_next_vfat_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino);
int64_t write_vfat(const char *path, const void *buffer, uint64_t count, uint64_t offset);
int truncate_vfat(const char *path, uint64_t length);
int create_vfat(const char *path, mode_t mode);
int mkdir_vfat(const char *path, mode_t mode);
int unlink_vfat(const char *path);
int rmdir_vfat(const char *path);
