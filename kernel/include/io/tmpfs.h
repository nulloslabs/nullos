#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/sys/types.h>
#include <freestanding/sys/stat.h>
#include <main/spinlocks.h>

#define TMPFS_MAX_NAME      128
#define TMPFS_MAX_INODES    4096
#define TMPFS_MAX_CHILDREN  256   // per directory
#define TMPFS_MAX_MOUNTS    8
#define TMPFS_LINK_MAX      256

typedef enum {
    TMPFS_NONE = 0,
    TMPFS_DIR,
    TMPFS_SOCK,
    TMPFS_REG,
    TMPFS_LNK,
} tmpfs_type_t;

typedef struct {
    bool         active;
    tmpfs_type_t type;
    char         name[TMPFS_MAX_NAME];
    mode_t       mode;
    uid_t        uid;
    gid_t        gid;
    int          parent;        // index of parent inode, -1 for mount root
    int          mount_idx;     // which tmpfs mount this inode lives under
    // TMPFS_REG:
    uint8_t     *data;
    uint64_t     size;
    uint64_t     capacity;
    // TMPFS_DIR:
    int          children[TMPFS_MAX_CHILDREN];
    int          child_count;
    // TMPFS_LNK:
    char         target[TMPFS_LINK_MAX];
    uint64_t     ino;
} tmpfs_inode_t;

extern tmpfs_inode_t tmpfs_inodes[TMPFS_MAX_INODES];
extern spinlock_t    tmpfs_lock;

int  create_tmpfs_root(const char *mount_path);
int  destroy_tmpfs_root(const char *mount_path);
bool match_tmpfs_mount(const char *abs_path);
int  resolve_tmpfs(const char *abs_path);                  // follow symlinks on last comp
int  resolve_tmpfs_nofollow(const char *abs_path);         // do NOT follow final symlink
int  resolve_tmpfs_parent(const char *abs_path, const char **last_out);
int  read_tmpfs_dirent(int dir_inode, int index, char *name, size_t name_size, uint8_t *type_out, uint64_t *ino_out);
int  make_tmpfs_dir(const char *abs_path, mode_t mode, uid_t uid, gid_t gid);
int  create_tmpfs_file(const char *abs_path, mode_t mode, uid_t uid, gid_t gid);
int  create_tmpfs_socket(const char *abs_path, mode_t mode, uid_t uid, gid_t gid);
int  make_tmpfs_symlink(const char *target, const char *abs_path, uid_t uid, gid_t gid);
int  remove_tmpfs(const char *abs_path);                   // unlink (file or symlink)
int  remove_tmpfs_dir(const char *abs_path);               // rmdir
int  rename_tmpfs(const char *old_abs, const char *new_abs);
int  link_tmpfs(const char *old_abs, const char *new_abs);
int  chmod_tmpfs(const char *abs_path, mode_t mode);
int  truncate_tmpfs(int inode, uint64_t size);
int64_t read_tmpfs(int inode, void *buf, uint64_t count, uint64_t offset);
int64_t write_tmpfs(int inode, const void *buf, uint64_t count, uint64_t offset);
bool stat_tmpfs(const char *abs_path, struct stat *st);
bool stat_tmpfs_nofollow(const char *abs_path, struct stat *st);
int  read_tmpfs_link(const char *abs_path, char *out, size_t out_size);
void init_tmpfs(void);
