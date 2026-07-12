#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/sys/types.h>

#define MAX_MODIFIED_FILES 1024

#define FT_FILE 1
#define FT_DIRECTORY 2
#define FT_EXECUTABLE 3
#define FT_SYMLINK 4

typedef struct {
    void* data;      // Pointer to the actual file content
    uint64_t size;   // Actual size of the file in bytes
    mode_t mode;
    uid_t uid;
    gid_t gid;
} initrd_file_t;

typedef struct {
    char path[256];
    void *data;
    uint64_t size;
    uint64_t capacity;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    bool is_active;
    // Tombstone: marks a path as deleted so read/stat don't fall through to
    // the initrd archive. Set when an archive-backed entry is removed (unlink/rmdir);
    // overlay entries are simply marked inactive instead.
    bool is_tombstone;
} modified_file_t;

typedef struct {
    char name[128];
    int type;
} directory_entry_t;

void get_absolute_path(const char *in, char *out_abs, size_t out_size);
void resolve_link_target(const char *base_path_abs, const char *link_target, char *out_abs, size_t out_size);
initrd_file_t read_initrd(const char *path);
initrd_file_t stat_initrd(const char *path);
initrd_file_t stat_initrd_nofollow(const char *path);
int write_initrd(const char *path, const void *data, uint64_t size, uint32_t mode, uid_t uid, gid_t gid);
int write_initrd_partial(const char *path, const void *data, uint64_t off, uint64_t count, uint32_t mode, uid_t uid, gid_t gid);
int mkdir_initrd(const char *path, mode_t mode, uid_t uid, gid_t gid);
int delete_initrd(const char *path);
int rmdir_initrd(const char *path);
int symlink_initrd(const char *target, const char *path, uid_t uid, gid_t gid);
int chmod_initrd(const char *path, mode_t mode);
int get_initrd_entry(int index, directory_entry_t* entry);
int next_initrd_child(int *index, const char *dir_norm, char *child_name, size_t child_name_size, uint8_t *child_type);
void init_initrd(void);
