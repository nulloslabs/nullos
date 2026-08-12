#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <main/sched.h>
#include <main/spinlocks.h>
#include <main/string.h>
#include <io/devtmpfs.h>
#include <io/extfs.h>
#include <io/initrd.h>
#include <io/procfs.h>
#include <io/tmpfs.h>
#include <io/vfs.h>
#include <mm/mm.h>

static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static uint64_t next_mount_id = 2;
static spinlock_t mount_lock = SPINLOCK_INIT;

typedef struct {
    const char *fs_type;
    int (*mount_fs)(const char *source, const char *path, unsigned long flags, const char *data);
    int (*unmount_fs)(const char *path);
} vfs_backend_t;

static int mount_tmpfs(const char *source, const char *path, unsigned long flags, const char *data) {
    (void)source;
    (void)flags;
    (void)data;
    return create_tmpfs_root(path);
}

static int unmount_tmpfs(const char *path) {
    return destroy_tmpfs_root(path);
}

static int mount_ext(const char *source, const char *path, unsigned long flags, const char *data) {
    if (!source || !source[0]) return -EINVAL;
    if (!(flags & MS_RDONLY)) return -EROFS;
    if (flags & ~(MS_RDONLY | MS_SILENT)) return -EOPNOTSUPP;
    if (data && data[0] && strcmp(data, "ro") != 0) return -EOPNOTSUPP;
    return mount_extfs(source, path);
}

static int unmount_ext(const char *path) {
    return unmount_extfs(path);
}

static const vfs_backend_t backends[] = {
    { "tmpfs",    mount_tmpfs, unmount_tmpfs },
    { "ext2",     mount_ext,   unmount_ext   },
    { "ext3",     mount_ext,   unmount_ext   },
    { "ext4",     mount_ext,   unmount_ext   },
    { "proc",     NULL,        NULL          },
    { "devtmpfs", NULL,        NULL          },
    { "devpts",   NULL,        NULL          },
};

static const vfs_backend_t *find_backend(const char *fs_type) {
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (strcmp(backends[i].fs_type, fs_type) == 0) return &backends[i];
    }
    return NULL;
}

static bool check_prefix(const char *path, const char *root, const char **relative) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0 || (path[root_len] != '/' && path[root_len] != '\0')) {
        return false;
    }
    if (relative) {
        const char *value = path + root_len;
        if (*value == '/') value++;
        *relative = value;
    }
    return true;
}

int register_vfs_mount(const char *source, const char *path, const char *fs_type, unsigned long flags) {
    if (!path || !path[0] || !fs_type || !fs_type[0]) return -EINVAL;
    if (strlen(path) >= VFS_PATH_MAX || strlen(fs_type) >= VFS_FS_TYPE_MAX || (source && strlen(source) >= VFS_SOURCE_MAX)) return -ENAMETOOLONG;

    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    int free_slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(mounts[i].path, path) != 0) continue;
        if (strcmp(mounts[i].fs_type, fs_type) == 0) {
            spin_unlock_irqrestore(&mount_lock, irq);
            return 0;
        }
        spin_unlock_irqrestore(&mount_lock, irq);
        return -EBUSY;
    }
    if (free_slot < 0) {
        spin_unlock_irqrestore(&mount_lock, irq);
        return -ENOMEM;
    }

    vfs_mount_t *mount = &mounts[free_slot];
    memset(mount, 0, sizeof(*mount));
    strncpy(mount->source, source && source[0] ? source : "none",
            sizeof(mount->source) - 1);
    strncpy(mount->path, path, sizeof(mount->path) - 1);
    strncpy(mount->fs_type, fs_type,
            sizeof(mount->fs_type) - 1);
    mount->flags = flags;
    mount->id = next_mount_id++;
    mount->active = true;
    spin_unlock_irqrestore(&mount_lock, irq);
    return 0;
}

int unregister_vfs_mount(const char *path, vfs_mount_t *removed) {
    if (!path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active || strcmp(mounts[i].path, path) != 0) continue;
        if (removed) *removed = mounts[i];
        memset(&mounts[i], 0, sizeof(mounts[i]));
        spin_unlock_irqrestore(&mount_lock, irq);
        return 0;
    }
    spin_unlock_irqrestore(&mount_lock, irq);
    return -ENOENT;
}

int mount_vfs(const char *source, const char *path, const char *fs_type, unsigned long flags, const char *data) {
    if (!path || !fs_type) return -EINVAL;

    vfs_mount_t existing;
    if (find_vfs_mount(path, &existing)) {
        return strcmp(existing.fs_type, fs_type) == 0 ? 0 : -EBUSY;
    }

    const vfs_backend_t *backend = find_backend(fs_type);
    if (!backend) return -ENODEV;

    int status = backend->mount_fs ? backend->mount_fs(source, path, flags, data) : 0;
    if (status < 0) return status;

    status = register_vfs_mount(source, path, fs_type, flags);
    if (status < 0 && backend->unmount_fs) backend->unmount_fs(path);
    return status;
}

int unmount_vfs(const char *path, int flags) {
    if (!path) return -EINVAL;
    if (flags != 0) return -EINVAL;

    vfs_mount_t mount;
    if (!find_vfs_mount(path, &mount)) return -ENOENT;
    const vfs_backend_t *backend = find_backend(mount.fs_type);
    int status = backend && backend->unmount_fs ? backend->unmount_fs(path) : 0;
    if (status < 0) return status;
    return unregister_vfs_mount(path, NULL);
}

int64_t read_vfs(const char *path, void *buf, uint64_t count, uint64_t offset) {
    if (!path || (!buf && count)) return -EINVAL;
    if (count == 0) return 0;
    if (check_vfs_device(path)) return -EACCES;

    if (is_procfs_path(path)) {
        proc_node_t node;
        int self = current_task_ptr ? current_task : -1;
        if (self < 0 || !resolve_procfs(path, self, &node)) return -ENOENT;
        if (is_procfs_dir(&node)) return -EISDIR;
        if (node.type == PROC_NODE_SYMLINK) return -EINVAL;
        char content[PROCFS_MAX_CONTENT];
        uint64_t size = get_procfs_content(&node, content);
        if (offset >= size) return 0;
        uint64_t bytes = count < size - offset ? count : size - offset;
        memcpy(buf, content + offset, bytes);
        return (int64_t)bytes;
    }

    if (is_tmpfs_dir(path)) {
        tmpfs_file_t file = read_tmpfs(path);
        if (!file.mode) return -ENOENT;
        if (S_ISDIR(file.mode)) return -EISDIR;
        if (offset >= file.size) return 0;
        uint64_t bytes = count < file.size - offset ? count : file.size - offset;
        memcpy(buf, (uint8_t *)file.data + offset, bytes);
        return (int64_t)bytes;
    }

    if (check_extfs_path(path)) return read_extfs(path, buf, count, offset);

    initrd_file_t file = read_initrd(path);
    if (!file.mode) return -ENOENT;
    if (S_ISDIR(file.mode)) return -EISDIR;
    if (offset >= file.size) return 0;
    uint64_t bytes = count < file.size - offset ? count : file.size - offset;
    memcpy(buf, (uint8_t *)file.data + offset, bytes);
    return (int64_t)bytes;
}

static int load_internal(const char *path, void **data, uint64_t *size, uint64_t max_size, int depth) {
    if (!path || !data || !size) return -EINVAL;
    if (depth >= 40) return -ELOOP;
    *data = NULL;
    *size = 0;

    uint64_t file_size;
    if (is_procfs_path(path)) {
        proc_node_t node;
        int self = current_task_ptr ? current_task : -1;
        if (self < 0 || !resolve_procfs(path, self, &node)) return -ENOENT;
        if (is_procfs_dir(&node)) return -EISDIR;
        if (node.type == PROC_NODE_SYMLINK) {
            char target[VFS_PATH_MAX];
            int status = read_procfs_link(&node, self, target, sizeof(target));
            if (status < 0) return status;
            char resolved[VFS_PATH_MAX];
            resolve_link_target(path, target, resolved, sizeof(resolved));
            return load_internal(resolved, data, size, max_size, depth + 1);
        }
        char content[PROCFS_MAX_CONTENT];
        file_size = get_procfs_content(&node, content);
        if (file_size > max_size) return -EFBIG;
        void *copy = malloc(file_size ? file_size : 1);
        if (!copy) return -ENOMEM;
        if (file_size) memcpy(copy, content, file_size);
        *data = copy;
        *size = file_size;
        return 0;
    }

    if (is_tmpfs_dir(path)) {
        tmpfs_file_t file = read_tmpfs(path);
        if (!file.mode) return -ENOENT;
        if (S_ISDIR(file.mode)) return -EISDIR;
        file_size = file.size;
    } else if (check_extfs_path(path)) {
        struct stat st;
        int status = stat_extfs(path, &st, true);
        if (status < 0) return status;
        if (S_ISDIR(st.st_mode)) return -EISDIR;
        if (st.st_size < 0) return -EIO;
        file_size = (uint64_t)st.st_size;
    } else {
        initrd_file_t file = read_initrd(path);
        if (!file.mode) return -ENOENT;
        if (S_ISDIR(file.mode)) return -EISDIR;
        file_size = file.size;
    }

    if (file_size > max_size) return -EFBIG;
    void *copy = malloc(file_size ? file_size : 1);
    if (!copy) return -ENOMEM;
    int64_t count = read_vfs(path, copy, file_size, 0);
    if (count < 0 || (uint64_t)count != file_size) {
        free(copy);
        return count < 0 ? (int)count : -EIO;
    }
    *data = copy;
    *size = file_size;
    return 0;
}

int load_vfs(const char *path, void **data, uint64_t *size, uint64_t max_size) {
    return load_internal(path, data, size, max_size, 0);
}

void build_vfs_path(const char *path, char *abs_path, size_t abs_size) {
    get_absolute_path(path, abs_path, abs_size);
}

bool find_vfs_mount(const char *path, vfs_mount_t *mount) {
    if (!path) return false;
    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active || strcmp(mounts[i].path, path) != 0) continue;
        if (mount) *mount = mounts[i];
        spin_unlock_irqrestore(&mount_lock, irq);
        return true;
    }
    spin_unlock_irqrestore(&mount_lock, irq);
    return false;
}

bool resolve_vfs_path(const char *path, vfs_mount_t *mount, char *relative_out, size_t relative_size) {
    if (!path) return false;
    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    int best = -1;
    size_t best_len = 0;
    const char *best_relative = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        const char *relative;
        if (!mounts[i].active || !check_prefix(path, mounts[i].path, &relative)) continue;
        size_t length = strlen(mounts[i].path);
        if (length < best_len) continue;
        best = i;
        best_len = length;
        best_relative = relative;
    }
    if (best >= 0) {
        if (mount) *mount = mounts[best];
        if (relative_out && relative_size) {
            strncpy(relative_out, best_relative, relative_size - 1);
            relative_out[relative_size - 1] = '\0';
        }
    }
    spin_unlock_irqrestore(&mount_lock, irq);
    return best >= 0;
}

bool match_vfs_path(const char *path, const char *fs_type, char *relative_out) {
    if (!path || !fs_type) return false;
    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    int best = -1;
    size_t best_len = 0;
    const char *best_relative = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        const char *relative;
        if (!mounts[i].active || strcmp(mounts[i].fs_type, fs_type) != 0 || !check_prefix(path, mounts[i].path, &relative)) continue;
        size_t length = strlen(mounts[i].path);
        if (length < best_len) continue;
        best = i;
        best_len = length;
        best_relative = relative;
    }
    if (best >= 0 && relative_out) {
        strncpy(relative_out, best_relative, VFS_PATH_MAX - 1);
        relative_out[VFS_PATH_MAX - 1] = '\0';
    }
    spin_unlock_irqrestore(&mount_lock, irq);
    return best >= 0;
}

bool check_vfs_device(const char *path) {
    char relative[VFS_PATH_MAX];
    return match_vfs_path(path, "devtmpfs", relative) && relative[0] != '\0' && device_exists_on_devtmpfs(relative);
}

bool find_vfs_submount(const char *parent_path, int index, char *name, size_t name_size) {
    if (!parent_path || index < 0 || !name || name_size == 0) return false;
    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    size_t parent_len = strlen(parent_path);
    int count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active || strncmp(mounts[i].path, parent_path, parent_len) != 0 || mounts[i].path[parent_len] != '/') continue;
        const char *rest = mounts[i].path + parent_len + 1;
        if (!rest[0] || strchr(rest, '/')) continue;
        if (count++ != index) continue;
        strncpy(name, rest, name_size - 1);
        name[name_size - 1] = '\0';
        spin_unlock_irqrestore(&mount_lock, irq);
        return true;
    }
    spin_unlock_irqrestore(&mount_lock, irq);
    return false;
}

uint64_t get_vfs_id(const char *path, bool *is_mount_root) {
    if (is_mount_root) *is_mount_root = path && strcmp(path, "/") == 0;
    if (!path) return 1;
    vfs_mount_t mount;
    if (!resolve_vfs_path(path, &mount, NULL, 0)) return 1;
    if (is_mount_root) *is_mount_root = strcmp(path, mount.path) == 0;
    return mount.id;
}

int list_vfs_mount(int index, char *out_line, size_t line_size) {
    if (index < 0 || !out_line || line_size == 0) return 0;
    out_line[0] = '\0';
    uint64_t irq;
    spin_lock_irqsave(&mount_lock, &irq);
    int count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) continue;
        if (count++ != index) continue;
        const char *parts[] = {
            mounts[i].source[0] ? mounts[i].source : "none", " ",
            mounts[i].path, " ", mounts[i].fs_type, " ",
            (mounts[i].flags & MS_RDONLY) ? "ro" : "rw", " 0 0"
        };
        size_t written = 0;
        for (size_t part = 0; part < sizeof(parts) / sizeof(parts[0]); part++) {
            for (const char *s = parts[part]; *s && written + 1 < line_size; s++)
                out_line[written++] = *s;
        }
        out_line[written] = '\0';
        spin_unlock_irqrestore(&mount_lock, irq);
        return (int)written;
    }
    spin_unlock_irqrestore(&mount_lock, irq);
    return 0;
}

// VFS file operations mirroring initrd/tmpfs APIs
// These functions route to the appropriate filesystem backend

vfs_file_t read_vfs_file(const char *path) {
    vfs_file_t result = {0};
    vfs_mount_t mount;
    char relative[VFS_PATH_MAX];
    
    if (resolve_vfs_path(path, &mount, relative, sizeof(relative))) {
        if (strcmp(mount.fs_type, "tmpfs") == 0) {
            tmpfs_file_t tmpfs_result = read_tmpfs(relative);
            result.inode = tmpfs_result.inode;
            result.data = tmpfs_result.data;
            result.size = tmpfs_result.size;
            result.mode = tmpfs_result.mode;
            result.uid = tmpfs_result.uid;
            result.gid = tmpfs_result.gid;
            result.atime = tmpfs_result.atime;
            result.mtime = tmpfs_result.mtime;
            result.ctime = tmpfs_result.ctime;
        } else if (strcmp(mount.fs_type, "initrd") == 0) {
            initrd_file_t initrd_result = read_initrd(relative);
            result.inode = initrd_result.inode;
            result.data = initrd_result.data;
            result.size = initrd_result.size;
            result.mode = initrd_result.mode;
            result.uid = initrd_result.uid;
            result.gid = initrd_result.gid;
            result.atime = initrd_result.atime;
            result.mtime = initrd_result.mtime;
            result.ctime = initrd_result.ctime;
            result.btime = initrd_result.btime;
        }
    } else {
        initrd_file_t initrd_result = read_initrd(path);
        if (initrd_result.mode) {
            result.inode = initrd_result.inode;
            result.data = initrd_result.data;
            result.size = initrd_result.size;
            result.mode = initrd_result.mode;
            result.uid = initrd_result.uid;
            result.gid = initrd_result.gid;
            result.atime = initrd_result.atime;
            result.mtime = initrd_result.mtime;
            result.ctime = initrd_result.ctime;
            result.btime = initrd_result.btime;
        }
    }
    
    return result;
}

vfs_file_t stat_vfs_file(const char *path) {
    vfs_file_t result = {0};
    vfs_mount_t mount;
    char relative[VFS_PATH_MAX];
    
    if (resolve_vfs_path(path, &mount, relative, sizeof(relative))) {
        if (strcmp(mount.fs_type, "tmpfs") == 0) {
            tmpfs_file_t tmpfs_result = stat_tmpfs(relative);
            result.inode = tmpfs_result.inode;
            result.data = tmpfs_result.data;
            result.size = tmpfs_result.size;
            result.mode = tmpfs_result.mode;
            result.uid = tmpfs_result.uid;
            result.gid = tmpfs_result.gid;
            result.atime = tmpfs_result.atime;
            result.mtime = tmpfs_result.mtime;
            result.ctime = tmpfs_result.ctime;
        } else if (strcmp(mount.fs_type, "initrd") == 0) {
            initrd_file_t initrd_result = stat_initrd(relative);
            result.inode = initrd_result.inode;
            result.data = initrd_result.data;
            result.size = initrd_result.size;
            result.mode = initrd_result.mode;
            result.uid = initrd_result.uid;
            result.gid = initrd_result.gid;
            result.atime = initrd_result.atime;
            result.mtime = initrd_result.mtime;
            result.ctime = initrd_result.ctime;
            result.btime = initrd_result.btime;
        }
    } else {
        initrd_file_t initrd_result = stat_initrd(path);
        if (initrd_result.mode) {
            result.inode = initrd_result.inode;
            result.data = initrd_result.data;
            result.size = initrd_result.size;
            result.mode = initrd_result.mode;
            result.uid = initrd_result.uid;
            result.gid = initrd_result.gid;
            result.atime = initrd_result.atime;
            result.mtime = initrd_result.mtime;
            result.ctime = initrd_result.ctime;
            result.btime = initrd_result.btime;
        }
    }
    
    return result;
}

int write_vfs_file(const char *path, const void *data, uint64_t size, mode_t mode, uid_t uid, gid_t gid) {
    vfs_mount_t mount;
    char relative[VFS_PATH_MAX];
    
    if (resolve_vfs_path(path, &mount, relative, sizeof(relative))) {
        if (strcmp(mount.fs_type, "tmpfs") == 0) {
            return write_tmpfs(relative, data, size, mode, uid, gid);
        } else if (strcmp(mount.fs_type, "initrd") == 0) {
            return write_initrd(relative, data, size, mode, uid, gid);
        }
    }
    return write_initrd(path, data, size, mode, uid, gid);
}

int mkdir_vfs(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    vfs_mount_t mount;
    char relative[VFS_PATH_MAX];
    
    if (resolve_vfs_path(path, &mount, relative, sizeof(relative))) {
        if (strcmp(mount.fs_type, "tmpfs") == 0) {
            return mkdir_tmpfs(relative, mode, uid, gid);
        } else if (strcmp(mount.fs_type, "initrd") == 0) {
            return mkdir_initrd(relative, mode, uid, gid);
        }
    }
    return mkdir_initrd(path, mode, uid, gid);
}

int unlink_vfs(const char *path) {
    vfs_mount_t mount;
    char relative[VFS_PATH_MAX];
    
    if (resolve_vfs_path(path, &mount, relative, sizeof(relative))) {
        if (strcmp(mount.fs_type, "tmpfs") == 0) {
            return delete_tmpfs(relative);
        } else if (strcmp(mount.fs_type, "initrd") == 0) {
            return delete_initrd(relative);
        }
    }
    return delete_initrd(path);
}

int rmdir_vfs(const char *path) {
    vfs_mount_t mount;
    char relative[VFS_PATH_MAX];
    
    if (resolve_vfs_path(path, &mount, relative, sizeof(relative))) {
        if (strcmp(mount.fs_type, "tmpfs") == 0) {
            return rmdir_tmpfs(relative);
        } else if (strcmp(mount.fs_type, "initrd") == 0) {
            return rmdir_initrd(relative);
        }
    }
    return rmdir_initrd(path);
}
