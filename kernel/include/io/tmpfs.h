#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <main/spinlocks.h>

#define TMPFS_MAX_NAME      128
#define TMPFS_MAX_INODES    4096
#define TMPFS_MAX_CHILDREN  256   // per directory
#define TMPFS_MAX_MOUNTS    8
#define TMPFS_LINK_MAX      256
#define TMPFS_MAX_FILE_SIZE (256ULL * 1024 * 1024)

typedef enum {
    TMPFS_NONE = 0,
    TMPFS_DIR,
    TMPFS_SOCK,
    TMPFS_REG,
    TMPFS_LNK,
} tmpfs_type_t;

typedef struct {
    int inode;
    char *name;
} tmpfs_child_t;

typedef struct {
    bool         active;
    tmpfs_type_t type;
    char         name[TMPFS_MAX_NAME];
    mode_t       mode;
    uid_t        uid;
    gid_t        gid;
    struct timespec atime;
    struct timespec btime;
    struct timespec mtime;
    struct timespec ctime;
    int          parent;        // index of parent inode, -1 for mount root
    int          mount_idx;     // which tmpfs mount this inode lives under
    // TMPFS_REG:
    uint8_t     *data;
    uint64_t     size;
    uint64_t     capacity;
    // TMPFS_DIR:
    tmpfs_child_t *children;
    int          child_count;
    int          child_capacity;
    // TMPFS_LNK:
    char        *target;
    uint64_t     ino;
} tmpfs_inode_t;

// Mirror of initrd_file_t: path-based read/stat return this.
typedef struct {
    ino_t    inode;
    void    *data;     // file content (TMPFS_REG only)
    uint64_t size;     // actual size in bytes
    mode_t   mode;
    uid_t    uid;
    gid_t    gid;
    struct timespec atime;
    struct timespec btime;
    struct timespec mtime;
    struct timespec ctime;
} tmpfs_file_t;

typedef struct { char name[128]; int type; } tmpfs_dirent_t;

extern tmpfs_inode_t *tmpfs_inodes[TMPFS_MAX_INODES];
extern spinlock_t    tmpfs_lock;

// Mount management (used by sys_mount/sys_umount2 only)
int  create_tmpfs_root(const char *mount_path);
int  destroy_tmpfs_root(const char *mount_path);
bool is_tmpfs_dir(const char *abs_path);

// Path-based API mirroring initrd exactly
tmpfs_file_t read_tmpfs(const char *path);
tmpfs_file_t stat_tmpfs(const char *path);
tmpfs_file_t stat_tmpfs_nofollow(const char *path);
int  write_tmpfs(const char *path, const void *data, uint64_t size, uint32_t mode, uid_t uid, gid_t gid);
int  write_tmpfs_partial(const char *path, const void *data, uint64_t off, uint64_t count, uint32_t mode, uid_t uid, gid_t gid);
int  mkdir_tmpfs(const char *path, mode_t mode, uid_t uid, gid_t gid);
int  delete_tmpfs(const char *path);
int  rmdir_tmpfs(const char *path);
int  symlink_tmpfs(const char *target, const char *path, uid_t uid, gid_t gid);
int  rename_tmpfs(const char *old_path, const char *new_path);
int  link_tmpfs(const char *old_path, const char *new_path);
int  chmod_tmpfs(const char *path, mode_t mode);
int  chown_tmpfs(const char *path, uid_t uid, gid_t gid, bool follow);
int  set_tmpfs_times(const char *path, struct timespec atime, bool set_atime, struct timespec mtime, bool set_mtime, bool follow);
int  truncate_tmpfs(const char *path, uint64_t size);
int  read_tmpfs_link(const char *path, char *out, size_t out_size);
int  next_tmpfs_child(int *index, const char *dir_norm, char *child_name, size_t child_name_size, uint8_t *child_type, ino_t *child_ino);
int  get_tmpfs_entry(int index, tmpfs_dirent_t *entry);
void init_tmpfs(void);
