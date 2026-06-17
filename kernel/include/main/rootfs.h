#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>
#include <freestanding/sys/types.h>

#define MAX_MODIFIED_FILES 128

#define FT_FILE 1
#define FT_DIRECTORY 2
#define FT_EXECUTABLE 3
#define FT_SYMLINK 4

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];     // Size is in Octal ASCII
    char mtime[12];
    char checksum[8];
    char typeflag[1];
    char linkname[100];
    char magic[6];     // "ustar"
    char version[2];
};

typedef struct {
    void* data;      // Pointer to the actual file content
    uint64_t size;   // Actual size of the file in bytes
    mode_t mode;
    uid_t uid;
    gid_t gid;
} rootfs_file_t;

typedef struct {
    char path[256];
    void *data;
    uint64_t size;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    bool is_active;
} modified_file_t;

typedef struct {
    char name[128];
    int type;
} directory_entry_t;

// Some public helpers
void get_absolute_path(const char *in, char *out_abs, size_t out_size);

rootfs_file_t read_rootfs(const char *path);
rootfs_file_t stat_rootfs(const char *path);
int write_rootfs(const char *path, const void *data, uint64_t size, uint32_t mode, uid_t uid, gid_t gid);
int delete_rootfs(const char *path);
int mkdir_rootfs(const char *path, mode_t mode, uid_t uid, gid_t gid);
int rmdir_rootfs(const char *path);
int symlink_rootfs(const char *target, const char *path, uid_t uid, gid_t gid);
int chmod_rootfs(const char *path, mode_t mode);
int get_rootfs_entry(int index, directory_entry_t* entry);
void init_rootfs(void);
