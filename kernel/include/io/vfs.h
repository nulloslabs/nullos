#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_MAX_MOUNTS 16
#define VFS_SOURCE_MAX 256
#define VFS_PATH_MAX 256
#define VFS_FS_TYPE_MAX 32

typedef struct {
    uint64_t id;
    char source[VFS_SOURCE_MAX];
    char path[VFS_PATH_MAX];
    char fs_type[VFS_FS_TYPE_MAX];
    unsigned long flags;
    bool active;
} vfs_mount_t;

int register_vfs_mount(const char *source, const char *path, const char *fs_type, unsigned long flags);
int unregister_vfs_mount(const char *path, vfs_mount_t *removed);
int mount_vfs(const char *source, const char *path, const char *fs_type, unsigned long flags, const char *data);
int unmount_vfs(const char *path, int flags);
int64_t read_vfs(const char *path, void *buf, uint64_t count, uint64_t offset);
int load_vfs(const char *path, void **data, uint64_t *size, uint64_t max_size);
void build_vfs_path(const char *path, char *abs_path, size_t abs_size);
bool find_vfs_mount(const char *path, vfs_mount_t *mount);
bool resolve_vfs_path(const char *path, vfs_mount_t *mount, char *relative_out, size_t relative_size);
bool match_vfs_path(const char *path, const char *fs_type, char *relative_out);
bool check_vfs_device(const char *path);
bool find_vfs_submount(const char *parent_path, int index, char *name, size_t name_size);
uint64_t get_vfs_id(const char *path, bool *is_mount_root);

int list_vfs_mount(int index, char *out_line, size_t line_size);
