#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/futex.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <sys/sysmacros.h>
#include <main/limine_req.h>
#include <main/elf.h>
#include <main/hostname.h>
#include <main/timekeeping.h>
#include <main/mp.h>
#include <main/fd.h>
#include <main/signal.h>
#include <main/string.h>
#include <io/fb.h>
#include <io/devices.h>
#include <io/devpts.h>
#include <io/pts_devices.h>
#include <io/terminal.h>
#include <io/tty.h>
#include <io/pty.h>
#include <io/time.h>
#include <io/sockets.h>
#include <io/net.h>
#include <io/unix_sockets.h>
#include <io/procfs.h>
#include <io/tmpfs.h>
#include <io/ext4.h>
#include <io/iso9660.h>
#include <io/vfat.h>
#include <io/gpt.h>
#include <io/usb.h>
#include <io/vfs.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/file.h>

futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];

spinlock_t stdin_lock = SPINLOCK_INIT;
spinlock_t futex_lock = SPINLOCK_INIT;

bool user_address_range_ok(uint64_t addr, uint64_t size) {
    if (addr >= USER_ADDR_MAX) return false;
    if (size > USER_ADDR_MAX - addr) return false;
    return true;
}

bool user_range_ok(vmm_context_t *ctx, uint64_t addr, uint64_t size) {
    return user_address_range_ok(addr, size) && vmm_user_range_valid(ctx, addr, size, false);
}

bool user_write_range_ok(vmm_context_t *ctx, uint64_t addr, uint64_t size) {
    return user_address_range_ok(addr, size) && vmm_user_range_valid(ctx, addr, size, true);
}

bool user_page_range_ok(uint64_t addr, uint64_t size, uint64_t *start, uint64_t *end) {
    if (!start || !end || size == 0 || !user_address_range_ok(addr, size)) return false;

    uint64_t last = addr + size - 1;
    uint64_t page_start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t page_end = (last & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;
    if (page_end < page_start || page_end > USER_ADDR_MAX) return false;

    *start = page_start;
    *end = page_end;
    return true;
}

bool fd_allows_read(const fd_entry_t *entry) {
    return entry && (entry->flags & O_ACCMODE) != O_WRONLY;
}

bool fd_allows_write(const fd_entry_t *entry) {
    int access_mode = entry ? (int)(entry->flags & O_ACCMODE) : O_RDONLY;
    return entry && (access_mode == O_WRONLY || access_mode == O_RDWR);
}

mode_t apply_current_umask(mode_t mode) {
    mode_t mask = current_task_ptr ? current_task_ptr->umask : 0022;
    return (mode & 07777) & ~mask;
}

bool user_range_is_mapped(vmm_context_t *ctx, uint64_t addr, uint64_t size) {
    uint64_t start;
    uint64_t end;
    if (!ctx || !user_page_range_ok(addr, size, &start, &end)) return false;
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t pte = get_vmm_pte(ctx, page);
        if (pte & (VMM_PRESENT | VMM_DEMAND)) return true;
    }
    return false;
}

static inline int fd_isset(const uint8_t *set, int fd) { return (set[fd / 8] >> (fd % 8)) & 1; }

static inline void fd_set_bit(uint8_t *set, int fd) { set[fd / 8] |= (uint8_t)(1u << (fd % 8)); }

static inline void fd_clr_bit(uint8_t *set, int fd) { set[fd / 8] &= ~(uint8_t)(1u << (fd % 8)); }

bool can_access_initrd(const initrd_file_t *file, int want_read, int want_write, int want_exec) {
    if (!file) return false;
    if (current_task_ptr && current_task_ptr->euid == 0) {
        if (want_exec && !S_ISDIR(file->mode) && !(file->mode & S_IXUGO)) return false;
        return true;
    }

    int shift = 0;
    if (current_task_ptr->fsuid == file->uid) {
        shift = 6;
    } else if (current_task_ptr->fsgid == file->gid) {
        shift = 3;
    }

    int perm = (file->mode >> shift) & 7;
    if (want_read && !(perm & 4)) return false;
    if (want_write && !(perm & 2)) return false;
    if (want_exec && !(perm & 1)) return false;
    return true;
}

bool can_access_stat_mode(const struct stat *st, int want_read, int want_write, int want_exec) {
    if (!st) return false;
    if (current_task_ptr && current_task_ptr->euid == 0) {
        if (want_exec && !S_ISDIR(st->st_mode) && !(st->st_mode & S_IXUGO)) return false;
        return true;
    }

    int shift = 0;
    if (current_task_ptr->fsuid == st->st_uid) {
        shift = 6;
    } else if (current_task_ptr->fsgid == st->st_gid) {
        shift = 3;
    }

    int perm = (st->st_mode >> shift) & 7;
    if (want_read && !(perm & 4)) return false;
    if (want_write && !(perm & 2)) return false;
    if (want_exec && !(perm & 1)) return false;
    return true;
}

static void normalize_path_str(char *path, size_t path_size) {
    char tmp[256];
    strlcpy(tmp, path, sizeof(tmp));

    const char *parts[64];
    int depth = 0;

    char *tok = tmp;
    while (*tok) {
        if (*tok == '/') { tok++; continue; }
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';

        if (strcmp(tok, ".") == 0) {
            // skip
        } else if (strcmp(tok, "..") == 0) {
            if (depth > 0) depth--;
        } else {
            if (depth < 64) parts[depth++] = tok;
        }

        if (slash) tok = slash + 1;
        else break;
    }

    if (depth == 0) { strlcpy(path, "/", 256); return; }

    char out[256];
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strlcat(out, "/", sizeof(out));
        strlcat(out, parts[i], sizeof(out));
    }
    strlcpy(path, out, path_size);
}

int proc_self_idx(void) {
    if (!current_task_ptr) return -1;
    return current_task;  // current_task is the running task's index
}

void resolve_path_symlinks_ex(const char *path, char *out, size_t out_size, bool follow_final) {
    char work[256];
    strncpy(work, path, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    int link_count = 0;
    const int MAX_LINKS = 40; // Linux ELOOP limit

    int pos = 0;
    if (work[0] == '/') pos = 1;

    while (work[pos] && link_count < MAX_LINKS) {
        while (work[pos] == '/') pos++;
        if (!work[pos]) break;

        int end = pos;
        while (work[end] && work[end] != '/') end++;

        // Skip . (current dir)
        if (end == pos + 1 && work[pos] == '.') {
            pos = end;
            if (work[pos] == '/') pos++;
            continue;
        }

        // Handle .. (parent dir)
        if (end == pos + 2 && work[pos] == '.' && work[pos + 1] == '.') {
            if (pos > 1) {
                pos--;
                while (pos > 1 && work[pos - 1] != '/') pos--;
            }
            size_t rest_len = strlen(work + end);
            memmove(work + pos, work + end, rest_len + 1);
            continue;
        }

        // If this is the final component and we should not follow it, stop.
        if (!follow_final && work[end] == '\0') {
            break;
        }

        char saved = work[end];
        work[end] = '\0';
        char prefix[256];
        strncpy(prefix, work, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
        initrd_file_t file = stat_initrd_nofollow(prefix);
        work[end] = saved;

        char link_target[256];
        bool followed = false;
        if (file.mode && S_ISLNK(file.mode) && file.data) {
            resolve_link_target(prefix, (const char *)file.data, link_target, sizeof(link_target));
            followed = true;
        } else if (is_procfs_path(prefix)) {
            int self = proc_self_idx();
            proc_node_t pn;
            if (resolve_procfs_nofollow(prefix, self, &pn) && pn.type == PROC_NODE_SYMLINK && pn.entry != PROC_LINK_FD && pn.entry != PROC_LINK_SELF) {
                char target[256];
                int tlen = read_procfs_link(&pn, self, target, sizeof(target));
                if (tlen >= 0) {
                    resolve_link_target(prefix, target, link_target, sizeof(link_target));
                    followed = true;
                }
            }
        }
        else if (check_iso9660_path(prefix)) {
            struct stat iso_st;
            if (stat_iso9660(prefix, &iso_st, false) == 0 && S_ISLNK(iso_st.st_mode)) {
                char raw_target[256];
                int tlen = read_iso9660_link(prefix, raw_target, sizeof(raw_target));
                if (tlen >= 0) {
                    // Rock ridge absolute targets are relative to the iso root, not /
                    char use_target[256];
                    char mount_root[256];
                    if (raw_target[0] == '/' && get_iso9660_mount_root(prefix, mount_root, sizeof(mount_root))) {
                        strncpy(use_target, mount_root, sizeof(use_target) - 1);
                        use_target[sizeof(use_target) - 1] = '\0';
                        strncat(use_target, raw_target, sizeof(use_target) - strlen(use_target) - 1);
                    } else {
                        strncpy(use_target, raw_target, sizeof(use_target) - 1);
                        use_target[sizeof(use_target) - 1] = '\0';
                    }
                    resolve_link_target(prefix, use_target, link_target, sizeof(link_target));
                    followed = true;
                }
            }
        }

        if (followed) {
            link_count++;

            char rebuilt[256];
            strncpy(rebuilt, link_target, sizeof(rebuilt) - 1);
            rebuilt[sizeof(rebuilt) - 1] = '\0';
            if (saved) {
                size_t cur_len = strlen(rebuilt);
                strncat(rebuilt, work + end, sizeof(rebuilt) - cur_len - 1);
            }
            normalize_path_str(rebuilt, sizeof(rebuilt));
            strncpy(work, rebuilt, sizeof(work) - 1);
            work[sizeof(work) - 1] = '\0';

            pos = 0;
            if (work[0] == '/') pos = 1;
        } else {
            pos = end;
            if (work[pos] == '/') pos++;
        }
    }

    strncpy(out, work, out_size - 1);
    out[out_size - 1] = '\0';
}

// Convenience: resolve ALL symlink components (used by open, chdir, etc.)
void resolve_path_symlinks(const char *path, char *out, size_t out_size) { resolve_path_symlinks_ex(path, out, out_size, true); }

int build_abs_path_at(int dirfd, const char *path, char *out, size_t out_size) {
    if (path[0] == '/') {
        strncpy(out, path, out_size - 1);
    } else {
        if (dirfd == AT_FDCWD) {
            strncpy(out, current_task_ptr->cwd, out_size - 1);
        } else {
            fd_entry_t *entry = get_fd(&current_task_ptr->fd_table, dirfd);
            if (!entry || !entry->open) return -EBADF;
            strncpy(out, entry->path, out_size - 1);
        }
        if (strcmp(out, "/") != 0)
            strncat(out, "/", out_size - strlen(out) - 1);
        strncat(out, path, out_size - strlen(out) - 1);
    }
    out[out_size - 1] = '\0';
    normalize_path_str(out, out_size);

    return 0;
}

void build_abs_path(const char *path, char *out, size_t out_size) { build_abs_path_at(AT_FDCWD, path, out, out_size); }

int copy_string_from_user(char *dest, const char *src, size_t capacity) {
    if (!dest || !src || capacity == 0) return -EFAULT;
    for (size_t i = 0; i < capacity; i++) {
        if (copy_from_user(&dest[i], &src[i], 1) < 0) return -EFAULT;
        if (dest[i] == '\0') return 0;
    }
    dest[capacity - 1] = '\0';
    return -ENAMETOOLONG;
}

int copy_from_user_strarray(char ***out_karray, const char **user_arr, size_t max_elements) {
    char **k_arr = malloc((max_elements + 1) * sizeof(char *));
    if (!k_arr) return -ENOMEM;

    if (!user_arr) {
        k_arr[0] = NULL;
        *out_karray = k_arr;
        return 0;
    }

    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)user_arr, sizeof(char *))) {
        free(k_arr);
        return -EFAULT;
    }

    size_t count = 0;
    while (count < max_elements) {
        char *u_ptr = NULL;
        uint64_t user_element_addr = (uint64_t)&user_arr[count];

        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)user_element_addr, sizeof(char *))) break;

        if (read_vmm(current_task_ptr->ctx, &u_ptr, user_element_addr, sizeof(char *)) < 0) break;

        if (!u_ptr) break;

        char *k_str = malloc(256);
        if (!k_str) {
            for (size_t i = 0; i < count; i++) free(k_arr[i]);
            free(k_arr);
            return -ENOMEM;
        }

        int string_status = copy_string_from_user(k_str, u_ptr, 256);
        if (string_status < 0) {
            free(k_str);
            for (size_t i = 0; i < count; i++) free(k_arr[i]);
            free(k_arr);
            return string_status;
        }

        k_arr[count++] = k_str;
    }

    k_arr[count] = NULL;
    *out_karray = k_arr;

    return (int)count;
}

int ptm_path_idx(const char *path) {
    if (path[0] != 'p' || path[1] != 't' || path[2] != 'm' || path[3] != ':') return -1;
    const char *n = path + 4;
    int idx = 0;
    while (*n >= '0' && *n <= '9') idx = idx * 10 + (*n++ - '0');
    return idx;
}

void free_strarray(char **arr, int count) { if (!arr) return; for (int i = 0; i < count; i++) free(arr[i]); free(arr); }

static struct timespec synthetic_fs_time(void) {
    static struct timespec timestamp;
    if (timestamp.tv_sec == 0 && timestamp.tv_nsec == 0) timestamp = time_get_realtime_ts();
    return timestamp;
}

static void stat_set_synthetic_times(struct stat *kst) {
    struct timespec timestamp = synthetic_fs_time();
    kst->st_atim = timestamp;
    kst->st_mtim = timestamp;
    kst->st_ctim = timestamp;
}

static void stat_set_shmem_directory_size(struct stat *kst, int child_count) {
    if (!kst || child_count < 0) return;
    kst->st_size = (2 + child_count) * SHMEM_BOGO_DIRENT_SIZE;
    kst->st_blocks = 0;
}

bool stat_virtual_device(const char *abs_path, struct stat *kst) {
    // build_abs_path_at already resolved intermediate symlinks;
    // resolve the final component too for virtual device lookup.
    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));

    char rel_path[256];
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs), // so devtmpfs would incorrectly match /dev/pts paths with rel="pts".
    if (is_devpts_path(resolved, rel_path)) {
        if (rel_path[0] == '\0') {
            kst->st_mode = S_IFDIR | 0755;
            kst->st_nlink = 2;
        } else if (devpts_device_exists(rel_path)) {
            kst->st_mode = S_IFCHR | 0620;
            kst->st_nlink = 1;
        } else {
            return false; // fall through to initrd (symlinks, etc.)
        }
        kst->st_uid = 0; kst->st_gid = 0; kst->st_size = 0;
        kst->st_blocks = 0; kst->st_blksize = 4096;
        stat_set_synthetic_times(kst);
        return true;
    }
    if (is_devtmpfs_path(resolved, rel_path)) {
        if (rel_path[0] == '\0') {
            // The /dev directory itself
            kst->st_mode = S_IFDIR | 0755;
            kst->st_nlink = 2;
        } else if (get_device_mode(rel_path, &kst->st_mode) == 0) {
            kst->st_nlink = 1;
        } else {
            return false; // let initrd handle it
        }
        kst->st_uid = 0; kst->st_gid = 0; kst->st_size = 0;
        kst->st_blocks = 0; kst->st_blksize = 4096;
        if (rel_path[0] == '\0') {
            int children = 0;
            while (get_devtmpfs_device_name(children)) children++;
            int submounts = 0;
            char sub_name[64];
            while (find_vfs_submount(resolved, submounts, sub_name, sizeof(sub_name))) submounts++;
            stat_set_shmem_directory_size(kst, children + submounts);
        }
        stat_set_synthetic_times(kst);
        return true;
    }
    return false;
}

// tmpfs equivalent of building kst from initrd_file_t (see sys_stat's manual
// initrd_file_t -> kst fill below). tmpfs.h only exposes the path-based
// tmpfs_file_t API (mirroring initrd exactly), so this is the local bridge
// to struct stat, same as the inline fill used for initrd.
bool stat_tmpfs_to_kst(const char *abs_path, struct stat *kst, bool follow) {
    tmpfs_file_t f = follow ? stat_tmpfs(abs_path) : stat_tmpfs_nofollow(abs_path);
    if (!f.mode) return false;
    memset(kst, 0, sizeof(*kst));
    kst->st_mode = f.mode;
    kst->st_uid = f.uid;
    kst->st_gid = f.gid;
    kst->st_size = f.size;
    kst->st_blocks = (f.size + 511) / 512;
    kst->st_blksize = 4096;
    kst->st_nlink = 1;
    kst->st_dev = 0;
    kst->st_ino = f.inode;
    kst->st_atim = f.atime;
    kst->st_mtim = f.mtime;
    kst->st_ctim = f.ctime;
    if (S_ISDIR(f.mode)) stat_set_shmem_directory_size(kst, count_tmpfs_children(abs_path));
    return true;
}

bool stat_initrd_to_kst(const char *abs_path, struct stat *kst, bool follow) {
    initrd_file_t f = follow ? stat_initrd(abs_path) : stat_initrd_nofollow(abs_path);
    if (!f.mode) return false;
    memset(kst, 0, sizeof(*kst));
    kst->st_mode = f.mode;
    kst->st_uid = f.uid;
    kst->st_gid = f.gid;
    kst->st_size = f.size;
    kst->st_blocks = (f.size + 511) / 512;
    kst->st_blksize = 4096;
    kst->st_nlink = 1;
    kst->st_dev = 1;
    kst->st_ino = f.inode;
    kst->st_atim = f.atime;
    kst->st_mtim = f.mtime;
    kst->st_ctim = f.ctime;
    if (S_ISDIR(f.mode)) stat_set_shmem_directory_size(kst, count_initrd_children(abs_path));
    return true;
}

bool stat_ext4_to_kst(const char *abs_path, struct stat *kst, bool follow) {
    if (!check_ext4_path(abs_path)) return false;
    return stat_ext4(abs_path, kst, follow) == 0;
}

bool stat_iso9660_to_kst(const char *abs_path, struct stat *kst, bool follow) {
    if (!check_iso9660_path(abs_path)) return false;
    return stat_iso9660(abs_path, kst, follow) == 0;
}

bool stat_vfat_to_kst(const char *abs_path, struct stat *kst, bool follow) {
    if (!check_vfat_path(abs_path)) return false;
    return stat_vfat(abs_path, kst, follow) == 0;
}

static int check_directory_access(const char *path, bool write) {
    struct stat st;
    if (0) {
    } else if (check_ext4_path(path)) {
        if (!stat_ext4_to_kst(path, &st, true)) return -ENOENT;
        if (write) return -EROFS;
    } else if (check_iso9660_path(path)) {
        if (!stat_iso9660_to_kst(path, &st, true)) return -ENOENT;
        if (write) return -EROFS;
    } else if (check_vfat_path(path)) {
        if (!stat_vfat_to_kst(path, &st, true)) return -ENOENT;
    } else if (is_tmpfs_dir(path)) {
        if (!stat_tmpfs_to_kst(path, &st, true)) return -ENOENT;
    } else {
        initrd_file_t dir = read_initrd(path);
        if (!dir.mode) return -ENOENT;
        memset(&st, 0, sizeof(st));
        st.st_mode = dir.mode;
        st.st_uid = dir.uid;
        st.st_gid = dir.gid;
    }
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    return can_access_stat_mode(&st, 0, write, 1) ? 0 : -EACCES;
}

int check_parent_access(const char *path, bool modify) {
    char parent[256];
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    size_t len = strlen(parent);
    while (len > 1 && parent[len - 1] == '/') parent[--len] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash) return -EINVAL;
    if (slash == parent) parent[1] = '\0';
    else *slash = '\0';

    bool immediate = modify;
    for (;;) {
        int status = check_directory_access(parent, immediate);
        if (status < 0) return status;
        if (strcmp(parent, "/") == 0) break;
        slash = strrchr(parent, '/');
        if (!slash || slash == parent) strlcpy(parent, "/", sizeof(parent));
        else *slash = '\0';
        immediate = false;
    }
    return 0;
}

bool stat_proc(const char *abs_path, const char *orig_path, struct stat *kst, bool follow_self) {
    if (!is_procfs_path(abs_path)) return false;
    int self = proc_self_idx();
    proc_node_t n;
    if (follow_self) {
        if (!resolve_procfs(abs_path, self, &n)) return false;
    } else {
        if (!resolve_procfs_nofollow_orig(abs_path, orig_path, self, &n)) return false;
    }

    kst->st_uid = current_task_ptr ? current_task_ptr->euid : 0;
    kst->st_gid = current_task_ptr ? current_task_ptr->egid : 0;
    kst->st_blocks = 0; kst->st_blksize = 4096; kst->st_nlink = 1;
    stat_set_synthetic_times(kst);

    if (is_procfs_dir(&n)) {
        kst->st_mode = S_IFDIR | 0555;  // dr-xr-xr-x
        kst->st_size = 0;
    } else if (n.type == PROC_NODE_SYMLINK) {
        char linkbuf[256];
        int linklen = read_procfs_link(&n, self, linkbuf, sizeof(linkbuf));
        if (follow_self && linklen >= 0) {
            char target_abs[256];
            if (linkbuf[0] == '/') {
                strncpy(target_abs, linkbuf, sizeof(target_abs) - 1);
                target_abs[sizeof(target_abs) - 1] = '\0';
            } else {
                resolve_link_target(abs_path, linkbuf, target_abs, sizeof(target_abs));
            }
            // Try virtual device first (e.g. /dev/tty1), then initrd.
            if (stat_virtual_device(target_abs, kst)) return true;
            if (stat_initrd_to_kst(target_abs, kst, true)) return true;
        }
        // lstat() or target not resolvable: report the synthetic symlink.
        kst->st_mode = S_IFLNK | 0777;  // lrwxrwxrwx (symlink)
        kst->st_size = (linklen < 0) ? 0 : (off_t)linklen;
    } else {
        // Regular procfs file (maps, mounts): size is the content length.
        kst->st_mode = S_IFREG | 0444;  // -r--r--r--
        char tmp[PROCFS_MAX_CONTENT];
        kst->st_size = get_procfs_content(&n, tmp);
    }
    return true;
}

int proc_open_common(char *abs_path, size_t abs_size, uint32_t flags) {
    if (!is_procfs_path(abs_path)) return 1;  // not procfs
    int self = proc_self_idx();
    proc_node_t n;
    if (!resolve_procfs(abs_path, self, &n)) return -ENOENT;

    // Directories open as regular directory fds (offset-tracked for getdents).
    if (is_procfs_dir(&n)) {
        return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_PROC, flags);
    }
    if (n.type == PROC_NODE_SYMLINK) {
        char target[256];
        int tlen = read_procfs_link(&n, self, target, sizeof(target));
        if (tlen < 0) return -ENOENT;
        char target_abs[256];
        if (target[0] == '/') {
            strncpy(target_abs, target, sizeof(target_abs) - 1);
            target_abs[sizeof(target_abs) - 1] = '\0';
        } else {
            // Relative targets (e.g. "self" -> "<pid>") resolve against /proc.
            resolve_link_target(abs_path, target, target_abs, sizeof(target_abs));
        }
        initrd_file_t f = read_initrd(target_abs);
        if (f.mode) {
            strncpy(abs_path, target_abs, abs_size - 1);
            abs_path[abs_size - 1] = '\0';
            return 1;  // fall through to initrd/devtmpfs/devpts open path
        }
        return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_PROC, flags);
    }
    // Regular procfs files are readable.
    if (n.entry == PROC_FILE_MAPS || n.entry == PROC_FILE_MOUNTS || n.entry == PROC_FILE_AUXV || n.entry == PROC_FILE_CPUINFO || n.entry == PROC_FILE_MEMINFO || n.entry == PROC_FILE_UPTIME || n.entry == PROC_FILE_ROOT_STAT || n.entry == PROC_FILE_LOADAVG || n.entry == PROC_FILE_STAT || n.entry == PROC_FILE_STATUS || n.entry == PROC_FILE_CMDLINE || n.entry == PROC_FILE_COMM) {
        return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_PROC, flags);
    }
    return -EACCES;
}

// tmpfs open helper.  Returns: >=0 = fd, <0 = -errno, 1 = "not tmpfs, fall through".
// Path-based throughout, mirroring the initrd open path exactly — no inode
// caching, no reaching into tmpfs_inodes[] directly.
int open_tmpfs_common(const char *abs_path, uint32_t flags, mode_t mode) {
    if (!is_tmpfs_dir(abs_path)) return 1;
    int parent_access = check_parent_access(abs_path, false);
    if (parent_access < 0) return parent_access;

    tmpfs_file_t f = stat_tmpfs(abs_path); // follows symlinks, like read_initrd/stat_initrd
    int want_write = (flags & O_ACCMODE) != O_RDONLY;
    int want_read = (flags & O_ACCMODE) != O_WRONLY;

    if (!f.mode) {
        // Does not exist yet
        if (!(flags & O_CREAT)) return -ENOENT;
        int access = check_parent_access(abs_path, true);
        if (access < 0) return access;
        int r = write_tmpfs(abs_path, NULL, 0, apply_current_umask(mode ? mode : 0644) | S_IFREG, current_task_ptr->fsuid, current_task_ptr->fsgid);
        if (r < 0) return r;
    } else {
        // O_EXCL: fail if exists
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
        // O_DIRECTORY: must be a directory
        if ((flags & O_DIRECTORY) && !S_ISDIR(f.mode)) return -ENOTDIR;
        // Directories open as directory fds (for getdents)
        if (S_ISDIR(f.mode)) {
            struct stat dst;
            if (!stat_tmpfs_to_kst(abs_path, &dst, true) || !can_access_stat_mode(&dst, 1, 0, 1)) return -EACCES;
            return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_TMPFS, flags);
        }
        // Permission check
        struct stat tst;
        if (stat_tmpfs_to_kst(abs_path, &tst, true)) {
            if (!can_access_stat_mode(&tst, want_read, want_write, 0)) return -EACCES;
        }
        // Permission is checked before a destructive truncate.
        if ((flags & O_TRUNC) && !want_write) return -EACCES;
        if ((flags & O_TRUNC) && S_ISREG(f.mode)) {
            int t = truncate_tmpfs(abs_path, 0);
            if (t < 0) return t;
        }
    }
    return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_TMPFS, flags);
}

// Read-only ext-family open helper. Returns 1 when the path is not on ext4.
int open_ext4_common(const char *abs_path, uint32_t flags) {
    if (!check_ext4_path(abs_path)) return 1;

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    if (want_write || (flags & (O_CREAT | O_TRUNC))) return -EROFS;

    struct stat st;
    int status = stat_ext4(abs_path, &st, true);
    if (status < 0) return status;

    if ((flags & O_DIRECTORY) && !S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (!can_access_stat_mode(&st, 1, 0, S_ISDIR(st.st_mode))) return -EACCES;
    return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_EXT4, flags);
}

// Read-only iso9660 open helper. Returns 1 when the path is not on iso9660.
int open_iso9660_common(const char *abs_path, uint32_t flags) {
    if (!check_iso9660_path(abs_path)) return 1;

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    if (want_write || (flags & (O_CREAT | O_TRUNC))) return -EROFS;

    struct stat st;
    int status = stat_iso9660(abs_path, &st, true);
    if (status < 0) return status;

    if ((flags & O_DIRECTORY) && !S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (!can_access_stat_mode(&st, 1, 0, S_ISDIR(st.st_mode))) return -EACCES;
    return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_ISO9660, flags);
}

int open_fat32_common(const char *abs_path, uint32_t flags) {
    if (!check_vfat_path(abs_path)) return 1;
    int parent_access = check_parent_access(abs_path, false);
    if (parent_access < 0) return parent_access;
    struct stat st;
    int status = stat_vfat(abs_path, &st, true);
    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);
    if (status < 0) {
        if (!(flags & O_CREAT)) return status;
        int access = check_parent_access(abs_path, true);
        if (access < 0) return access;
        int r = create_vfat(abs_path, 0644);
        if (r < 0) return r;
        status = stat_vfat(abs_path, &st, true);
        if (status < 0) return status;
    } else {
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
        if ((flags & O_DIRECTORY) && !S_ISDIR(st.st_mode)) return -ENOTDIR;
        if (S_ISDIR(st.st_mode)) {
            if (!can_access_stat_mode(&st, 1, 0, 1)) return -EACCES;
            return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_VFAT, flags);
        }
        if (!can_access_stat_mode(&st, want_read, want_write, 0)) return -EACCES;
        if ((flags & O_TRUNC) && !want_write) return -EACCES;
        if ((flags & O_TRUNC) && S_ISREG(st.st_mode)) {
            int t = truncate_vfat(abs_path, 0);
            if (t < 0) return t;
        }
    }
    if (!can_access_stat_mode(&st, want_read, want_write, 0)) return -EACCES;
    return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_VFAT, flags);
}

uint64_t resolve_futex_key(uint32_t *uaddr, syscall_frame_t *frame) {
    if (!uaddr || !user_range_ok(current_task_ptr->ctx, (uint64_t)uaddr, sizeof(uint32_t))) {
        frame->rax = (uint64_t)-EFAULT;
        return 0;
    }
    uint64_t phys = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr);
    if (!phys) {
        frame->rax = (uint64_t)-EFAULT;
        return 0;
    }
    return phys;
}

void wait_futex(syscall_frame_t *frame, uint64_t phys, uint32_t val, struct timespec *timeout_ptr, uint32_t bitset, bool absolute_timeout) {
    if (bitset == 0) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t deadline_us = 0;
    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timespec))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        struct timespec ts;
        if (read_vmm(current_task_ptr->ctx, &ts, (uint64_t)timeout_ptr, sizeof(struct timespec)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
            frame->rax = (uint64_t)-EINVAL; return;
        }
        uint64_t timeout_us = (uint64_t)ts.tv_sec * 1000000ULL
                            + (uint64_t)ts.tv_nsec / 1000ULL;
        if (absolute_timeout) {
            deadline_us = timeout_us;
        } else {
            deadline_us = time_get_realtime_us() + timeout_us;
        }

        if (deadline_us != 0 && time_get_realtime_us() >= deadline_us) {
            frame->rax = (uint64_t)-ETIMEDOUT; return;
        }
    }

    uint64_t irq_flags;
    spin_lock_irqsave(&futex_lock, &irq_flags);

    uint32_t cur_val = 0;
    if (read_vmm(current_task_ptr->ctx, &cur_val, (uint64_t)(frame->rdi), sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }
    if (cur_val != val) {
        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)-EAGAIN;
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].state == FW_FREE) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)-ENOMEM;
        return;
    }

    futex_waiters[slot].state       = FW_WAITING;
    futex_waiters[slot].phys_addr   = phys;
    futex_waiters[slot].task_idx    = current_task;
    futex_waiters[slot].bitset      = bitset;
    futex_waiters[slot].deadline_us = deadline_us;

    current_task_ptr->state = TASK_STOPPED;
    current_task_ptr->stopped_by_signal = 0;

    spin_unlock_irqrestore(&futex_lock, irq_flags);

    while (1) {
        spin_unlock(&sched_lock);
        yield_sched();
        spin_lock(&sched_lock);

        spin_lock_irqsave(&futex_lock, &irq_flags);
        if (futex_waiters[slot].state != FW_WAITING) {
            break;
        }
        if (signal_pending()) {
            break;
        }
        current_task_ptr->state = TASK_STOPPED;
        spin_unlock_irqrestore(&futex_lock, irq_flags);
    }

    int wake_state = futex_waiters[slot].state;
    futex_waiters[slot].state = FW_FREE;
    spin_unlock_irqrestore(&futex_lock, irq_flags);

    if (wake_state == FW_TIMED_OUT) {
        // With no safety net, FW_TIMED_OUT can only be reached when the caller
        // supplied a real timeout (deadline_us != 0 implies timeout_ptr != NULL).
        frame->rax = (uint64_t)-ETIMEDOUT;
    } else if (signal_pending()) {
        frame->rax = (uint64_t)-EINTR;
    } else {
        frame->rax = 0;
    }
}

int wake_futex(uint64_t phys, uint32_t max_wake, uint32_t bitset) {
    if (bitset == 0) return 0;

    int woken = 0;
    uint64_t irq_flags;
    spin_lock_irqsave(&futex_lock, &irq_flags);

    for (int i = 0; i < MAX_FUTEX_WAITERS && (uint32_t)woken < max_wake; i++) {
        if (futex_waiters[i].state != FW_WAITING) continue;
        if (futex_waiters[i].phys_addr != phys) continue;
        if (!(futex_waiters[i].bitset & bitset)) continue;

        futex_waiters[i].state = FW_WOKEN;
        int idx = futex_waiters[i].task_idx;
        if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
            tasks[idx]->state = TASK_READY;
        woken++;
    }

    spin_unlock_irqrestore(&futex_lock, irq_flags);
    return woken;
}

int fill_rlimit(int resource, rlimit_t *lim) {
    switch (resource) {
        case RLIMIT_NOFILE:
            lim->rlim_cur = FD_MAX;
            lim->rlim_max = FD_MAX;
            return 0;
        case RLIMIT_CPU:
        case RLIMIT_FSIZE:
        case RLIMIT_DATA:
        case RLIMIT_STACK:
        case RLIMIT_CORE:
        case RLIMIT_RSS:
        case RLIMIT_AS:
            lim->rlim_cur = RLIM_INFINITY;
            lim->rlim_max = RLIM_INFINITY;
            return 0;
        default:
            return -EINVAL;
    }
}

uint16_t emit_dirent64(uint64_t bufp, uint64_t *written, uint64_t buflen, uint64_t ino, uint64_t off, uint8_t type, const char *name) {
    size_t namelen = strlen(name);
    uint16_t reclen = DIRENT64_ALIGN(DIRENT64_HEADER_SIZE + namelen + 1);
    if (*written + reclen > buflen) return 0;

    // Build the record in a stack buffer (max ~270 bytes — safe).
    uint8_t rec[512];
    memset(rec, 0, sizeof(rec));
    memcpy(rec, &ino, 8);
    memcpy(rec + 8, &off, 8);
    memcpy(rec + 16, &reclen, 2);
    rec[18] = type;
    memcpy(rec + DIRENT64_HEADER_SIZE, name, namelen);
    // null terminator and padding already zeroed

    (void)write_vmm(current_task_ptr->ctx, bufp + *written, rec, reclen);
    *written += reclen;
    return reclen;
}

uint16_t emit_dirent(uint64_t bufp, uint64_t *written, uint64_t buflen, uint64_t ino, uint64_t off, uint8_t type, const char *name) {
    size_t namelen = strlen(name);
    // d_type is the last byte of the record; total = 19 + namelen + 1, aligned to 8
    uint16_t raw = (uint16_t)(19 + namelen + 1);
    uint16_t reclen = DIRENT64_ALIGN(raw);
    if (*written + reclen > buflen) return 0;

    uint8_t rec[512];
    memset(rec, 0, sizeof(rec));
    memcpy(rec, &ino, 8);
    memcpy(rec + 8, &off, 8);
    memcpy(rec + 16, &reclen, 2);
    memcpy(rec + 19, name, namelen);
    rec[reclen - 1] = type;  // d_type is the very last byte

    (void)write_vmm(current_task_ptr->ctx, bufp + *written, rec, reclen);
    *written += reclen;
    return reclen;
}

void resolve_dir_for_readdir(const char *fd_path, char *prefix_out, size_t prefix_size, char *abs_out, size_t abs_size) {
    // Follow symlinks by reading through read_initrd and tracing the chain.
    char resolved[256];
    strncpy(resolved, fd_path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        initrd_file_t file = stat_initrd_nofollow(resolved);
        if (!file.mode) break; // doesn't exist at all
        if (!S_ISLNK(file.mode)) break; // not a symlink, done

        // Resolve symlink target relative to the symlink's parent directory
        const char *target = (const char *)file.data;
        if (!target) break;

        char link_abs[256];
        resolve_link_target(resolved, target, link_abs, sizeof(link_abs));
        strncpy(resolved, link_abs, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';

        // Normalize the resolved path
        normalize_path_str(resolved, sizeof(resolved));
    }

    // Hand back the absolute path (next_initrd_child builds its own prefix).
    if (abs_out) {
        strncpy(abs_out, resolved, abs_size - 1);
        abs_out[abs_size - 1] = '\0';
    }
    strncpy(prefix_out, resolved, prefix_size - 1);
    prefix_out[prefix_size - 1] = '\0';
}

int tty_rel_to_idx(const char *rel) {
    if (strcmp(rel, "console") == 0)
        return 0;
    if (strncmp(rel, "tty", 3) != 0)
        return -1;
    /* bare "/dev/tty" = controlling terminal */
    if (rel[3] == '\0') {
        int idx = current_task_ptr->ctty_idx;
        return (idx >= 0) ? idx : 1;
    }
    /* "/dev/tty0" is the foreground virtual terminal. */
    if (rel[3] == '0' && rel[4] == '\0') return keyboard_tty;
    /* "/dev/ttyN" */
    if (rel[3] >= '1' && rel[3] <= '7' && rel[4] == '\0')
        return rel[3] - '0';
    return -1;
}

int pty_rel_to_idx(const char *rel) {
    int idx = 0;
    const char *p = rel;

    if (!rel || *rel == '\0') return -1;
    while (*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0' || idx < 0 || idx >= NUM_PTYS) return -1;
    return idx;
}

int ioctl_tty_idx(fd_entry_t *entry) {
    if (current_task_ptr->ctty_idx >= 0) return current_task_ptr->ctty_idx;

    if (entry && (entry->type == FD_DEV || entry->type == FD_STREAM)) {
        char rel[256];
        if (entry->type == FD_STREAM) {
            return 1;
        }
        if (is_devtmpfs_path(entry->path, rel)) {
            int idx = tty_rel_to_idx(rel);
            if (idx >= 0) return idx;
        } else if (is_devpts_path(entry->path, rel)) {
            int idx = pty_rel_to_idx(rel);
            if (idx >= 0) return 100 + idx;
        }
    }

    return -1;
}

static int64_t read_dev_tty(char *kbuf, uint64_t count, int tty_idx) {
    tty_t *t = get_tty(tty_idx);
    if (!t) return (int64_t)-ENODEV;
    if (t->fg_pgrp > 0 && current_task_ptr->pgid != t->fg_pgrp) {
        signal_tty_pgrp(tty_idx, SIGTTIN);
        return (int64_t)-EINTR;
    }

    tcflag_t lflags = t->termios.c_lflag;
    uint64_t irq;

    if (!(lflags & ICANON)) {
        uint64_t total = 0;
        while (total < count) {
            // Try to grab whatever bytes are in the ring buffer right now.
            spinlock_t *lk = &tty_lock;
            spin_lock_irqsave(lk, &irq);
            int got = read_tty_ring(&t->input, kbuf + total, (int)(count - total));
            spin_unlock_irqrestore(lk, irq);

            if (got > 0) {
                total += (uint64_t)got;
                // In raw mode, a single read() should not block once it has
                // at least one byte — return what we have.
                break;
            }

            if (t->termios.c_cc[VMIN] == 0) {
                return (int64_t)total;
            }

            if (signal_pending()) return (int64_t)-EINTR;

            if (current_task_ptr->state == TASK_STOPPED) {
                spin_unlock(&sched_lock);
                do { yield_sched(); } while (current_task_ptr->state == TASK_STOPPED);
                spin_lock(&sched_lock);
                return (int64_t)-EINTR;
            }

            let_current_task_sleep(1000);
        }
        return (int64_t)total;
    }

    char *sbuf = current_task_ptr->stdin_buf;
    int *sbuf_len = &current_task_ptr->stdin_buf_len;
    int *sbuf_pos = &current_task_ptr->stdin_buf_pos;

    spin_lock_irqsave(&stdin_lock, &irq);
    if (*sbuf_pos < *sbuf_len) {
        // A full cooked line is already buffered from a prior read.
        uint64_t avail = (uint64_t)(*sbuf_len - *sbuf_pos);
        uint64_t to_copy = (count < avail) ? count : avail;
        memcpy(kbuf, sbuf + *sbuf_pos, to_copy);
        *sbuf_pos += (int)to_copy;
        spin_unlock_irqrestore(&stdin_lock, irq);
        return (int64_t)to_copy;
    }

    *sbuf_len = 0;
    *sbuf_pos = 0;
    spin_unlock_irqrestore(&stdin_lock, irq);

    while (1) {
        // Get one character from the ring buffer, blocking if needed.
        char c = 0;
        while (1) {
            spinlock_t *lk = &tty_lock;
            spin_lock_irqsave(lk, &irq);
            int got = read_tty_ring(&t->input, &c, 1);
            spin_unlock_irqrestore(lk, irq);
            if (got > 0) break;
            if (signal_pending()) {
                // Flush the per-task canonical buffer so partial input
                // doesn't leak into the next read() after signal delivery.
                spin_lock_irqsave(&stdin_lock, &irq);
                *sbuf_len = 0;
                *sbuf_pos = 0;
                spin_unlock_irqrestore(&stdin_lock, irq);
                return (int64_t)-EINTR;
            }
            // Asynchronous stop (SIGTSTP from Ctrl+Z) sets our state to
            // TASK_STOPPED without a pending bit, so signal_pending()
            // misses it. Detect it here, yield until SIGCONT, then surface
            // the interruption to the caller.
            if (current_task_ptr->state == TASK_STOPPED) {
                spin_unlock(&sched_lock);
                do { yield_sched(); } while (current_task_ptr->state == TASK_STOPPED);
                spin_lock(&sched_lock);
                spin_lock_irqsave(&stdin_lock, &irq);
                *sbuf_len = 0;
                *sbuf_pos = 0;
                spin_unlock_irqrestore(&stdin_lock, irq);
                return (int64_t)-EINTR;
            }
            // Yield properly: release sched_lock so isr32 is allowed to
            // call schedule() (it skips switching while sched_lock is held).
            let_current_task_sleep(1000);
        }

        spin_lock_irqsave(&stdin_lock, &irq);

        if (c == '\b' || c == 127) {
            if (*sbuf_len > 0) {
                (*sbuf_len)--;
                if (lflags & ECHO) printf("\b \b");
            }
            spin_unlock_irqrestore(&stdin_lock, irq);
            continue;
        }
        if (c == '\r') {
            // ICRNL: translate CR -> NL (standard terminal behaviour)
            tcflag_t iflags = t->termios.c_iflag;
            if (iflags & ICRNL) c = '\n';
        }
        if (c == '\n' || c == '\r') {
            if (lflags & ECHO) putchar(c);
            if (*sbuf_len < TASK_STDIN_BUF_SIZE) sbuf[(*sbuf_len)++] = c;
            spin_unlock_irqrestore(&stdin_lock, irq);
            break;
        }

        if (*sbuf_len < TASK_STDIN_BUF_SIZE - 1) {
            if (lflags & ECHO) putchar(c);
            sbuf[(*sbuf_len)++] = c;
        }
        spin_unlock_irqrestore(&stdin_lock, irq);
    }

    // Deliver the cooked line (up to count bytes).
    spin_lock_irqsave(&stdin_lock, &irq);
    uint64_t avail = (uint64_t)(*sbuf_len - *sbuf_pos);
    uint64_t to_copy = (count < avail) ? count : avail;
    memcpy(kbuf, sbuf + *sbuf_pos, to_copy);
    *sbuf_pos += (int)to_copy;
    spin_unlock_irqrestore(&stdin_lock, irq);
    return (int64_t)to_copy;
}

static int select_check_fd(int fd) {
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry || !entry->open) return -1;

    int result = 0;

    // readable?
    if (entry->type == FD_STREAM) {
        int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 1;
        tty_t *t = get_tty(tty_idx);
        if (t && get_tty_ring_count(&t->input) > 0) result |= 1;
        // In canonical (cooked) mode, read_dev_tty() drains the ring into the
        // per-task stdin_buf.  select must also report readable when that
        // buffer has unconsumed data, otherwise bash's readline sees ret=0
        // and goes to sleep even though a full line is buffered.
        if (current_task_ptr->stdin_buf_pos < current_task_ptr->stdin_buf_len)
            result |= 1;
    } else if (entry->type == FD_DEV) {
        char rel[256];
        int tty_idx = -1;
        if (is_devtmpfs_path(entry->path, rel)) {
            tty_idx = tty_rel_to_idx(rel);
        } else if (is_devpts_path(entry->path, rel)) {
            tty_idx = current_task_ptr->ctty_idx;
        }
        if (tty_idx >= 0) {
            tty_t *t = get_tty(tty_idx);
            if (t && get_tty_ring_count(&t->input) > 0) result |= 1;
            if (current_task_ptr->stdin_buf_pos < current_task_ptr->stdin_buf_len)
                result |= 1;
        } else {
            result |= 1;
        }
    } else if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        if (idx >= 0) {
            pty_t *p = get_pty(idx);
            if (p && p->allocated && get_tty_ring_count(&p->s2m) > 0) result |= 1;
        }
    } else if (entry->type == FD_PIPE) {
        unix_handle_t *h = (unix_handle_t *)entry->handle;
        if (h && h->in && !h->rd_shutdown) {
            uint64_t irq;
            spin_lock_irqsave(&h->in->lock, &irq);
            if (h->in->len > 0 || h->in->writers == 0) result |= 1;
            spin_unlock_irqrestore(&h->in->lock, irq);
        }
    } else if (entry->type == FD_SOCKET) {
        poll_net_device();
        if (is_socket_readable((socket_t *)entry->handle)) result |= 1;
    } else {
        result |= 1;
    }

    // writable (always for now, like poll)
    result |= 2;

    return result;
}

static int sleep_clock_status(int clock_id) {
    switch (clock_id) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_BOOTTIME:
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_BOOTTIME_ALARM:
    case CLOCK_TAI:
        return -EOPNOTSUPP;
    default:
        return -EINVAL;
    }
}

static ktime_t sleep_clock_now_ns(int clock_id) {
    uint64_t now_us = clock_id == CLOCK_REALTIME ? time_get_realtime_us() : get_monotonic_time_us();
    if (now_us >= (uint64_t)INT64_MAX / 1000ULL) return INT64_MAX;
    return (ktime_t)(now_us * 1000ULL);
}

static ktime_t sleep_timespec_to_ns(const struct timespec *time) {
    if ((uint64_t)time->tv_sec >= (uint64_t)(INT64_MAX / 1000000000LL)) return INT64_MAX;
    return (ktime_t)time->tv_sec * 1000000000LL + time->tv_nsec;
}

static ktime_t sleep_ns_add(ktime_t left, ktime_t right) {
    if (right > INT64_MAX - left) return INT64_MAX;
    return left + right;
}

static struct timespec sleep_ns_to_timespec(ktime_t ns) {
    struct timespec result = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
    return result;
}

task_t *find_priority_task(int which, id_t who) {
    if (which != PRIO_PROCESS) return NULL;
    if (who == 0 || who == (id_t)current_task_ptr->pid) return current_task_ptr;
    return task_by_pid((pid_t)who);
}

int prepare_unix_socket_path(uint8_t *addr, uint32_t *addrlen, bool binding) {
    if (!addr || !addrlen || *addrlen < sizeof(sa_family_t) + 1) return -EINVAL;
    sockaddr_un_t *un = (sockaddr_un_t *)addr;
    if (un->sun_family != AF_UNIX && un->sun_family != AF_LOCAL) return 0;
    if (un->sun_path[0] == '\0') return 0;
    size_t supplied = *addrlen - sizeof(sa_family_t);
    if (supplied > sizeof(un->sun_path)) supplied = sizeof(un->sun_path);
    size_t path_len = strnlen(un->sun_path, supplied);
    if (path_len == supplied) return -ENAMETOOLONG;
    char path[sizeof(un->sun_path)];
    memcpy(path, un->sun_path, path_len + 1);
    char absolute[256];
    get_absolute_path(path, absolute, sizeof(absolute));
    if (strlen(absolute) >= sizeof(un->sun_path)) return -ENAMETOOLONG;
    int access = check_parent_access(absolute, binding);
    if (access < 0) return access;
    if (!binding) {
        struct stat st;
        if (!stat_tmpfs_to_kst(absolute, &st, false)) return -ECONNREFUSED;
        if (!S_ISSOCK(st.st_mode)) return -ENOTSOCK;
        if (!can_access_stat_mode(&st, 0, 1, 0)) return -EACCES;
    }
    memset(un->sun_path, 0, sizeof(un->sun_path));
    strlcpy(un->sun_path, absolute, sizeof(un->sun_path));
    *addrlen = sizeof(sa_family_t) + strlen(un->sun_path) + 1;
    return 0;
}

int reject_procfs_mutation(const char *path) {
    if (check_ext4_path(path)) return -EROFS;
    if (check_iso9660_path(path)) return -EROFS;
    if (is_procfs_path(path)) return -EROFS;
    return 0;
}

int reject_virtual_removal(const char *path) {
    int status = reject_procfs_mutation(path);
    if (status < 0) return status;
    if (is_devtmpfs_path(path, NULL) || is_devpts_path(path, NULL)) return -EPERM;
    return 0;
}

int change_path_ownership(const char *path, uid_t uid, gid_t gid, bool follow) {
    if (current_task_ptr->euid != 0) return -EPERM;
    if (check_ext4_path(path)) return -EROFS;
    if (check_iso9660_path(path)) return -EROFS;
    if (check_vfat_path(path)) return 0;
    if (is_devtmpfs_path(path, NULL) || is_devpts_path(path, NULL) || is_procfs_path(path)) return -EPERM;
    if (is_tmpfs_dir(path)) return chown_tmpfs(path, uid, gid, follow);
    return chown_initrd(path, uid, gid, follow);
}

int set_path_times(const char *path, struct timespec atime, bool set_atime, struct timespec mtime, bool set_mtime, bool follow) {
    struct stat st = {0};
    bool is_virtual = stat_virtual_device(path, &st);
    bool is_tmpfs = !is_virtual && stat_tmpfs_to_kst(path, &st, follow);
    bool is_ext4 = !is_virtual && !is_tmpfs && stat_ext4_to_kst(path, &st, follow);
    bool is_iso9660 = !is_virtual && !is_tmpfs && !is_ext4 && stat_iso9660_to_kst(path, &st, follow);
    bool is_vfat = !is_virtual && !is_tmpfs && !is_ext4 && !is_iso9660 && stat_vfat_to_kst(path, &st, follow);
    bool is_proc = !is_virtual && !is_tmpfs && !is_ext4 && !is_iso9660 && !is_vfat && stat_proc(path, path, &st, follow);
    bool is_initrd = !is_virtual && !is_tmpfs && !is_ext4 && !is_iso9660 && !is_vfat && !is_proc && stat_initrd_to_kst(path, &st, follow);
    if (!is_virtual && !is_tmpfs && !is_ext4 && !is_iso9660 && !is_vfat && !is_proc && !is_initrd) return -ENOENT;
    if (!set_atime && !set_mtime) return 0;

    bool owner = current_task_ptr->fsuid == 0 || current_task_ptr->fsuid == st.st_uid;
    if (!owner && !can_access_stat_mode(&st, 0, 1, 0)) return -EACCES;
    if (is_ext4 || is_iso9660 || is_virtual || is_proc) return -EROFS;
    if (is_vfat) return 0;
    if (is_tmpfs) return set_tmpfs_times(path, atime, set_atime, mtime, set_mtime, follow);
    return set_initrd_times(path, atime, set_atime, mtime, set_mtime, follow);
}

int get_utimens_times(const struct timespec *user_times, struct timespec *atime, bool *set_atime, struct timespec *mtime, bool *set_mtime) {
    struct timespec now = time_get_realtime_ts();
    if (!user_times) { *atime = *mtime = now; *set_atime = *set_mtime = true; return 0; }

    struct timespec times[2];
    if (copy_from_user(times, user_times, sizeof(times)) < 0) return -EFAULT;
    if ((times[0].tv_nsec < 0 || times[0].tv_nsec >= 1000000000L) && times[0].tv_nsec != UTIME_NOW && times[0].tv_nsec != UTIME_OMIT) return -EINVAL;
    if ((times[1].tv_nsec < 0 || times[1].tv_nsec >= 1000000000L) && times[1].tv_nsec != UTIME_NOW && times[1].tv_nsec != UTIME_OMIT) return -EINVAL;

    *set_atime = times[0].tv_nsec != UTIME_OMIT;
    *set_mtime = times[1].tv_nsec != UTIME_OMIT;
    *atime = times[0].tv_nsec == UTIME_NOW ? now : times[0];
    *mtime = times[1].tv_nsec == UTIME_NOW ? now : times[1];
    return 0;
}

static int epoll_check_ready(int watched_fd, uint32_t req_events) {
    fd_entry_t *entry = get_current_fd(watched_fd);
    if (!entry || !entry->open) return -1;

    int result = 0;

    // Check readable
    if (req_events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLPRI)) {
        if (entry->type == FD_STREAM) {
            int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 1;
            tty_t *t = get_tty(tty_idx);
            if (t && get_tty_ring_count(&t->input) > 0) result |= 1;
            if (current_task_ptr->stdin_buf_pos < current_task_ptr->stdin_buf_len)
                result |= 1;
        } else if (entry->type == FD_DEV) {
            char rel[256];
            int tty_idx = -1;
            if (is_devtmpfs_path(entry->path, rel)) {
                tty_idx = tty_rel_to_idx(rel);
            } else if (is_devpts_path(entry->path, rel)) {
                tty_idx = current_task_ptr->ctty_idx;
            }
            if (tty_idx >= 0) {
                tty_t *t = get_tty(tty_idx);
                if (t && get_tty_ring_count(&t->input) > 0) result |= 1;
                if (current_task_ptr->stdin_buf_pos < current_task_ptr->stdin_buf_len)
                    result |= 1;
            } else {
                result |= 1;
            }
        } else if (entry->type == FD_PTY_MASTER) {
            int idx = ptm_path_idx(entry->path);
            if (idx >= 0) {
                pty_t *p = get_pty(idx);
                if (p && p->allocated && get_tty_ring_count(&p->s2m) > 0) result |= 1;
            }
        } else if (entry->type == FD_PIPE || entry->type == FD_SOCKET) {
            unix_handle_t *h = (unix_handle_t *)entry->handle;
            if (h && h->in && !h->rd_shutdown) {
                uint64_t irq;
                spin_lock_irqsave(&h->in->lock, &irq);
                if (h->in->len > 0 || h->in->writers == 0) result |= 1;
                spin_unlock_irqrestore(&h->in->lock, irq);
            }
            if (h && h->kind == UH_SOCKET && h->listening) {
                if (h->pending_len > 0) result |= 1;
            }
        } else {
            result |= 1; // files, procfs, etc: always readable
        }
    }

    // Check writable
    if (req_events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
        if (entry->type == FD_PIPE || entry->type == FD_SOCKET) {
            unix_handle_t *h = (unix_handle_t *)entry->handle;
            if (h && h->out && !h->wr_shutdown) {
                uint64_t irq;
                spin_lock_irqsave(&h->out->lock, &irq);
                if (h->out->len < UNIX_BUF_SIZE) result |= 2;
                spin_unlock_irqrestore(&h->out->lock, irq);
            } else if (h && h->wr_shutdown) {
                // write end closed: report writable so caller sees POLLOUT + subsequent write fails
                result |= 2;
            }
        } else {
            result |= 2; // everything else: always writable
        }
    }

    return result;
}

// Build epoll_event from a readiness check result
int epoll_collect(epoll_instance_t *epi, struct epoll_event *out, int maxevents) {
    int count = 0;
    for (int i = 0; i < epi->count && count < maxevents; i++) {
        epoll_interest_t *interest = &epi->interests[i];
        int status = epoll_check_ready(interest->watched_fd, interest->events);

        if (status < 0) continue; // fd was closed, skip (lazy cleanup)

        uint32_t ready = 0;
        if (status & 1) ready |= EPOLLIN;
        if (status & 2) ready |= EPOLLOUT;

        // EPOLLRDHUP: readable from a shutdown pipe/socket read-end
        // (handled by EPOLLIN above, also set EPOLLRDHUP if requested)
        if ((interest->events & EPOLLRDHUP) && (status & 1))
            ready |= EPOLLRDHUP;

        // Always report EPOLLERR/EPOLLHUP if requested
        ready |= (interest->events & (EPOLLERR | EPOLLHUP));

        // Mask to only events the caller registered
        ready &= interest->events | EPOLLERR | EPOLLHUP;
        // Actually: report ERR/HUP unconditionally per Linux convention
        ready |= (EPOLLERR | EPOLLHUP);
        ready &= interest->events | EPOLLERR | EPOLLHUP;

        if (!ready) continue;

        // EPOLLONESHOT: only report once, then disable
        if (interest->events & EPOLLONESHOT) {
            if (interest->oneshot_reported) continue;
            interest->oneshot_reported = true;
        }

        out[count].events = ready;
        out[count].data   = interest->data;
        count++;
    }
    return count;
}

int epoll_find_interest(epoll_instance_t *epi, int fd) {
    for (int i = 0; i < epi->count; i++) {
        if (epi->interests[i].watched_fd == fd) return i;
    }
    return -1;
}

int64_t do_select(int nfds, uint8_t *k_read, uint8_t *k_write, uint8_t *k_except, uint8_t *out_read, uint8_t *out_write, uint8_t *out_except, int out_bytes, int64_t timeout_us) {
    int count = 0;

    // First pass: non-blocking check
    for (int fd = 0; fd < nfds; fd++) {
        int check = 0;
        if (k_read   && fd_isset(k_read,   fd)) check |= 1;
        if (k_write  && fd_isset(k_write,  fd)) check |= 2;
        if (k_except && fd_isset(k_except, fd)) check |= 4;
        if (!check) continue;

        int status = select_check_fd(fd);
        if (status < 0) {
            // bad fd => treat as ready so caller sees EBADF-ish behavior
            if (k_read   && fd_isset(k_read,   fd)) fd_set_bit(out_read,   fd);
            if (k_write  && fd_isset(k_write,  fd)) fd_set_bit(out_write,  fd);
            count++;
            continue;
        }
        int hit = 0;
        if ((check & 1) && (status & 1)) { fd_set_bit(out_read, fd); hit = 1; }
        if ((check & 2) && (status & 2)) { fd_set_bit(out_write, fd); hit = 1; }
        // exceptfds: no special conditions yet, always 0
        if (hit) count++;
    }

    if (count > 0 || timeout_us == 0) {
        return count;
    }

    // Blocking loop
    uint64_t start = get_monotonic_time_us();
    while (1) {
        if (signal_pending()) return -EINTR;

        count = 0;
        if (out_read) memset(out_read, 0, out_bytes);
        if (out_write) memset(out_write, 0, out_bytes);
        if (out_except) memset(out_except, 0, out_bytes);

        for (int fd = 0; fd < nfds; fd++) {
            int check = 0;
            if (k_read   && fd_isset(k_read,   fd)) check |= 1;
            if (k_write  && fd_isset(k_write,  fd)) check |= 2;
            if (k_except && fd_isset(k_except, fd)) check |= 4;
            if (!check) continue;

            int status = select_check_fd(fd);
            if (status < 0) {
                if (k_read   && fd_isset(k_read,   fd)) fd_set_bit(out_read,   fd);
                if (k_write  && fd_isset(k_write,  fd)) fd_set_bit(out_write,  fd);
                count++;
                continue;
            }
            int hit = 0;
            if ((check & 1) && (status & 1)) { fd_set_bit(out_read, fd); hit = 1; }
            if ((check & 2) && (status & 2)) { fd_set_bit(out_write, fd); hit = 1; }
            if (hit) count++;
        }

        if (count > 0) {
            return count;
        }
        if (timeout_us > 0 && (int64_t)(get_monotonic_time_us() - start) >= timeout_us) {
            return 0;
        }

        let_current_task_sleep(1000);
    }
}

uint64_t do_read(int fd, void *buf, size_t count) {
    // Validate user buffer
    if (count > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, count)) { return (uint64_t)-EFAULT; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { return (uint64_t)-EBADF; }
    if (!fd_allows_read(entry)) { return (uint64_t)-EBADF; }

    if (entry->type == FD_STREAM) {
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        // For FD_STREAM, use the process's controlling terminal if set
        // This handles cases where a process has been assigned a TTY via TIOCSCTTY
        int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 1;
        // If ctty_idx is not set, try to parse the device path (e.g. /dev/tty1).
        if (current_task_ptr->ctty_idx < 0) {
            const char *path = entry->path;
            if (strstr(path, "tty")) {
                const char *p = strstr(path, "tty");
                if (p && p[3] >= '0' && p[3] <= '7') {
                    tty_idx = p[3] - '0';
                }
            }
        }
        int64_t got = read_dev_tty((char *)kbuf, count, tty_idx);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
        free(kbuf);
        return (uint64_t)got;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (is_devtmpfs_path(entry->path, rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }

            int tty_idx = tty_rel_to_idx(rel);
            if (tty_idx >= 0) {
                char local_buf[4096];
                uint8_t *kbuf = (count <= sizeof(local_buf)) ? (uint8_t*)local_buf : malloc(count);
                if (!kbuf) { return (uint64_t)-ENOMEM; }
                int64_t got = read_dev_tty((char *)kbuf, count, tty_idx);
                if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
                if (kbuf != (uint8_t*)local_buf) free(kbuf);
                return (uint64_t)got;
            }

            char local_buf[4096];
            uint8_t *kbuf = (count <= sizeof(local_buf)) ? (uint8_t*)local_buf : malloc(count);
            if (!kbuf) { return (uint64_t)-ENOMEM; }
            res = read_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, res) < 0) { res = (uint64_t)-EFAULT; } else if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != (uint8_t*)local_buf) free(kbuf);
        } else if (is_devpts_path(entry->path, rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
            char local_buf[4096];
            uint8_t *kbuf = (count <= sizeof(local_buf)) ? (uint8_t*)local_buf : malloc(count);
            if (!kbuf) { return (uint64_t)-ENOMEM; }
            res = read_pts_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, res) < 0) { res = (uint64_t)-EFAULT; } else if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != (uint8_t*)local_buf) free(kbuf);
        } else {
            return (uint64_t)-ENODEV;
        }
        return res;
    }

    if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        int got = read_pty_master(idx, (char *)kbuf, (int)count);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got) < 0) got = -EFAULT;
        free(kbuf);
        return (got < 0) ? (uint64_t)-EIO : (uint64_t)got;
    }

    if (entry->type == FD_PIPE) {
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        int64_t got = read_unix_handle((unix_handle_t *)entry->handle, kbuf, count, entry->flags);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got) < 0) got = -EFAULT;
        free(kbuf);
        return (uint64_t)got;
    }

    if (entry->type == FD_SOCKET) {
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        socket_t *sock = (socket_t *)entry->handle;
        int64_t got = -EBADF;
        if (sock && sock->ops && sock->ops->read) {
            got = sock->ops->read(sock, kbuf, count, entry->flags);
        }
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got) < 0) got = -EFAULT;
        free(kbuf);
        return (uint64_t)got;
    }

    if (entry->type == FD_VFAT) {
        if (count == 0) { return 0; }
        if (count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        char local_buf[4096];
        uint8_t *kbuf = count <= sizeof(local_buf) ? (uint8_t *)local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        int64_t got = read_vfat(entry->path, kbuf, count, entry->offset);
        if (got >= 0) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
            else entry->offset += (uint64_t)got;
        }
        if (kbuf != (uint8_t *)local_buf) free(kbuf);
        return (uint64_t)got;
    }
    if (entry->type == FD_PROC || entry->type == FD_TMPFS || entry->type == FD_EXT4 || entry->type == FD_ISO9660 || entry->type == FD_FILE) {
        if (count == 0) { return 0; }
        if (count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        char local_buf[4096];
        uint8_t *kbuf = count <= sizeof(local_buf) ? (uint8_t *)local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        int64_t got = read_vfs(entry->path, kbuf, count, entry->offset);
        if (got >= 0) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
            else entry->offset += (uint64_t)got;
        }
        if (kbuf != (uint8_t *)local_buf) free(kbuf);
        return (uint64_t)got;
    }

    return (uint64_t)-EBADF;
}

uint64_t do_write(int fd, const void *buf, size_t count) {
    int tty_idx;

    // Validate user buffer pointer
    if (!buf) { return (uint64_t)-EINVAL; }
    if (count > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)buf, count)) { return (uint64_t)-EFAULT; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { return (uint64_t)-EBADF; }
    if (!fd_allows_write(entry)) { return (uint64_t)-EBADF; }

    uint8_t local_buf[4096];

    if (entry->type == FD_EXT4) { return (uint64_t)-EROFS; }
    if (entry->type == FD_ISO9660) { return (uint64_t)-EROFS; }

    if (entry->type == FD_VFAT) {
        if (entry->flags & O_APPEND) {
            struct stat st;
            if (stat_vfat(entry->path, &st, true) == 0) entry->offset = st.st_size;
        }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }
        int64_t w = write_vfat(entry->path, kbuf, count, entry->offset);
        if (kbuf != local_buf) free(kbuf);
        if (w >= 0) entry->offset += w;
        return (uint64_t)w;
    }



    if (entry->type == FD_STREAM) {
        tty_idx = current_task_ptr->ctty_idx >= 0 && current_task_ptr->ctty_idx < NUM_TTYS ? current_task_ptr->ctty_idx : keyboard_tty;
        uint64_t processed = 0;
        while (processed < count) {
            poll_usb_hcds();
            if (signal_pending()) break;
            uint64_t chunk = count - processed;
            if (chunk > sizeof(local_buf)) chunk = sizeof(local_buf);
            if (read_vmm(current_task_ptr->ctx, local_buf, (uint64_t)buf + processed, chunk) < 0) { return (uint64_t)-EFAULT; }
            write_terminal_tty(tty_idx, (const char *)local_buf, chunk, false);
            processed += chunk;
        }
        return !processed && signal_pending() ? (uint64_t)-EINTR : processed;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (is_devtmpfs_path(entry->path, rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
            uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
            if (!kbuf) { return (uint64_t)-ENOMEM; }
            if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }
            res = write_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != local_buf) free(kbuf);
        } else if (is_devpts_path(entry->path, rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
            uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
            if (!kbuf) { return (uint64_t)-ENOMEM; }
            if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }
            res = write_pts_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != local_buf) free(kbuf);
        } else {
            return (uint64_t)-ENODEV;
        }
        return res;
    }

    if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }
        int w = write_pty_master(idx, (const char *)kbuf, (int)count);
        if (kbuf != local_buf) free(kbuf);
        return (w < 0) ? (uint64_t)-EIO : (uint64_t)w;
    }

    if (entry->type == FD_PIPE) {
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }
        int64_t w = write_unix_handle((unix_handle_t *)entry->handle, kbuf, count, entry->flags);
        if (kbuf != local_buf) free(kbuf);
        return (uint64_t)w;
    }

    if (entry->type == FD_SOCKET) {
        if (count == 0 || count > MAX_IO_COUNT) { return (uint64_t)-EINVAL; }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }
        socket_t *sock = (socket_t *)entry->handle;
        int64_t w = -EBADF;
        if (sock && sock->ops && sock->ops->write) {
            w = sock->ops->write(sock, kbuf, count, entry->flags);
        }
        if (kbuf != local_buf) free(kbuf);
        return (uint64_t)w;
    }

    if (entry->type == FD_TMPFS) {
        tmpfs_file_t tf = read_tmpfs(entry->path);

        // O_APPEND: write at current end of file
        if (entry->flags & O_APPEND) entry->offset = tf.size;

        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { return (uint64_t)-ENOMEM; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }

        int res = write_tmpfs_partial(entry->path, kbuf, entry->offset, count, tf.mode ? tf.mode : 0644, tf.mode ? tf.uid : current_task_ptr->euid, tf.mode ? tf.gid : current_task_ptr->egid);
        if (kbuf != local_buf) free(kbuf);
        if (res < 0) { return (uint64_t)res; }
        entry->offset += count;
        return count;
    }

    // O_APPEND: every write goes to the current end of file
    if (entry->flags & O_APPEND) {
        initrd_file_t file = read_initrd(entry->path);
        entry->offset = file.size;
    }

    uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
    if (!kbuf) { return (uint64_t)-ENOMEM; }
    if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); return (uint64_t)-EFAULT; }

    int res = write_initrd_partial(entry->path, kbuf, entry->offset, count);
    if (kbuf != local_buf) free(kbuf);

    if (res < 0) { return (uint64_t)res; }
    entry->offset += count;
    return count;
}

int do_clock_nanosleep(int clock_id, int flags, const struct timespec *req, struct timespec *rem) {
    int clock_status = sleep_clock_status(clock_id);
    if (clock_status < 0) return clock_status;
    if (flags & ~TIMER_ABSTIME) return -EINVAL;

    struct timespec request;
    if (!req || copy_from_user(&request, req, sizeof(request)) < 0) return -EFAULT;
    if (request.tv_sec < 0 || request.tv_nsec < 0 || request.tv_nsec >= 1000000000L) return -EINVAL;

    bool absolute = (flags & TIMER_ABSTIME) != 0;
    int wait_clock = absolute ? clock_id : CLOCK_MONOTONIC;
    ktime_t request_ns = sleep_timespec_to_ns(&request);
    ktime_t deadline = absolute ? request_ns : sleep_ns_add(sleep_clock_now_ns(wait_clock), request_ns);

    while (1) {
        ktime_t now_ns = sleep_clock_now_ns(wait_clock);
        if (now_ns >= deadline) break;

        if (signal_pending()) {
            if (!absolute && rem) {
                struct timespec remaining = sleep_ns_to_timespec(deadline - now_ns);
                if (copy_to_user(rem, &remaining, sizeof(remaining)) < 0) return -EFAULT;
            }
            return -EINTR;
        }

        ktime_t remaining_ns = deadline - now_ns;
        uint64_t remaining_us = ((uint64_t)remaining_ns + 999) / 1000;
        uint64_t now_us = get_monotonic_time_us();
        current_task_ptr->sleep_deadline_us = remaining_us > UINT64_MAX - now_us ? UINT64_MAX : now_us + remaining_us;
        current_task_ptr->state = TASK_SLEEPING;
        spin_unlock(&sched_lock);
        yield_sched();
        spin_lock(&sched_lock);
    }

    current_task_ptr->sleep_deadline_us = 0;
    return 0;
}

int do_epoll_create1(int flags) {
    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;

    epoll_instance_t *epi = malloc(sizeof(epoll_instance_t));
    if (!epi) return -ENOMEM;
    memset(epi, 0, sizeof(*epi));
    epi->refcount = 1;

    uint32_t fd_flags = 0;
    if (flags & EPOLL_CLOEXEC) fd_flags |= O_CLOEXEC;
    int fd = alloc_fd_handle(&current_task_ptr->fd_table, "epoll", FD_EPOLL, fd_flags, epi);
    if (fd < 0) {
        free(epi);
        return fd;
    }
    return fd;
}

int do_epoll_wait(syscall_frame_t *frame, int64_t timeout_us, uint64_t sigmask_arg) {
    int epfd      = (int)frame->rdi;
    struct epoll_event *user_events = (struct epoll_event *)frame->rsi;
    int maxevents = (int)frame->rdx;

    if (maxevents <= 0) { frame->rax = (uint64_t)-EINVAL; return -1; }
    if (maxevents > MAX_EPOLL_INTERESTS) maxevents = MAX_EPOLL_INTERESTS;

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return -1;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) { frame->rax = (uint64_t)-EBADF; return -1; }

    if (!user_events || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_events, maxevents * sizeof(struct epoll_event))) {
        frame->rax = (uint64_t)-EFAULT; return -1;
    }

    // Lazy cleanup: compact the interest list by removing entries whose fd is closed
    {
        int j = 0;
        for (int i = 0; i < epi->count; i++) {
            fd_entry_t *e = get_current_fd(epi->interests[i].watched_fd);
            if (e && e->open) {
                if (j != i) epi->interests[j] = epi->interests[i];
                j++;
            }
        }
        if (j != epi->count) epi->count = j;
    }

    // Allocate kernel buffer for collected events
    struct epoll_event *k_events = malloc(maxevents * sizeof(struct epoll_event));
    if (!k_events) { frame->rax = (uint64_t)-ENOMEM; return -1; }

    // Atomically swap signal mask if requested (like pselect6)
    uint64_t old_blocked = current_task_ptr->blocked_signals;
    if (sigmask_arg) {
        uint64_t ss_ptr = 0;
        size_t ss_len = 0;
        if (user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)sigmask_arg, 16)) {
            if (read_vmm(current_task_ptr->ctx, &ss_ptr,  sigmask_arg,     8) < 0) { free(k_events); frame->rax = (uint64_t)-EFAULT; return -1; }
            if (read_vmm(current_task_ptr->ctx, &ss_len,  sigmask_arg + 8, 8) < 0) { free(k_events); frame->rax = (uint64_t)-EFAULT; return -1; }
        }
        if (ss_ptr && ss_len == 8) {
            uint64_t new_mask = 0;
            if (user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)ss_ptr, 8)) {
                if (read_vmm(current_task_ptr->ctx, &new_mask, ss_ptr, 8) < 0) { free(k_events); frame->rax = (uint64_t)-EFAULT; return -1; }
            }
            new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            current_task_ptr->blocked_signals = new_mask;
        }
    }

    // Non-blocking poll
    int count = epoll_collect(epi, k_events, maxevents);
    if (count > 0 || timeout_us == 0) {
        current_task_ptr->blocked_signals = old_blocked;
        if (count > 0 && copy_to_user(user_events, k_events, count * sizeof(struct epoll_event)) < 0) count = -EFAULT;
        free(k_events);
        frame->rax = (uint64_t)count;
        return 0;
    }

    // Blocking loop
    uint64_t start = get_monotonic_time_us();
    while (1) {
        if (signal_pending()) {
            current_task_ptr->blocked_signals = old_blocked;
            free(k_events);
            frame->rax = (uint64_t)-EINTR;
            return -1;
        }

        count = epoll_collect(epi, k_events, maxevents);
        if (count > 0) {
            current_task_ptr->blocked_signals = old_blocked;
            if (copy_to_user(user_events, k_events, count * sizeof(struct epoll_event)) < 0) count = -EFAULT;
            free(k_events);
            frame->rax = (uint64_t)count;
            return 0;
        }

        if (timeout_us > 0 && (int64_t)(get_monotonic_time_us() - start) >= timeout_us) {
            current_task_ptr->blocked_signals = old_blocked;
            free(k_events);
            frame->rax = 0;
            return 0;
        }

        // Yield CPU and retry
        let_current_task_sleep(1000);
    }
}

int timeval_to_us(const struct timeval *tv, uint64_t *out) {
    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return -EINVAL;
    if ((uint64_t)tv->tv_sec > (UINT64_MAX - (uint64_t)tv->tv_usec) / 1000000ULL) return -EINVAL;
    *out = (uint64_t)tv->tv_sec * 1000000ULL + (uint64_t)tv->tv_usec;
    return 0;
}

int timespec_to_us(const struct timespec *ts, int64_t *out) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) return -EINVAL;
    if ((uint64_t)ts->tv_sec > ((uint64_t)INT64_MAX - (uint64_t)ts->tv_nsec / 1000ULL) / 1000000ULL) return -EINVAL;
    *out = (int64_t)((uint64_t)ts->tv_sec * 1000000ULL + (uint64_t)ts->tv_nsec / 1000ULL);
    return 0;
}

void fill_real_itimer(task_t *task, struct itimerval *value) {
    uint64_t now = time_get_realtime_us();
    uint64_t remaining = 0;
    if (task->real_timer_deadline_us > now) remaining = task->real_timer_deadline_us - now;

    memset(value, 0, sizeof(*value));
    value->it_value.tv_sec = (time_t)(remaining / 1000000ULL);
    value->it_value.tv_usec = (suseconds_t)(remaining % 1000000ULL);
    value->it_interval.tv_sec = (time_t)(task->real_timer_interval_us / 1000000ULL);
    value->it_interval.tv_usec = (suseconds_t)(task->real_timer_interval_us % 1000000ULL);
}

int copy_from_user(void *kdest, const void *usrc, size_t size) {
    if (!usrc) return -EFAULT;
    if ((uint64_t)usrc >= USER_ADDR_MAX || size > USER_ADDR_MAX - (uint64_t)usrc) return -EFAULT;
    if (!kdest || size == 0) return 0;
    if (!vmm_user_range_valid(current_task_ptr->ctx, (uint64_t)usrc, size, false)) return -EFAULT;

    return read_vmm(current_task_ptr->ctx, kdest, (uint64_t)usrc, size);
}

int copy_to_user(const void *udest, const void *ksrc, size_t size) {
    if (!udest) return -EFAULT;
    if ((uint64_t)udest >= USER_ADDR_MAX || size > USER_ADDR_MAX - (uint64_t)udest) return -EFAULT;
    if (!ksrc || size == 0) return 0;
    if (!vmm_user_range_valid(current_task_ptr->ctx, (uint64_t)udest, size, true)) return -EFAULT;

    return write_vmm(current_task_ptr->ctx, (uint64_t)udest, ksrc, size);
}

static void check_signals_context(syscall_frame_t *frame, bool from_syscall, bool sched_locked) {
    if (!current_task_ptr) return;
    if (current_task_ptr->pending_signals == 0) return;

    for (int i = 1; i < 32; i++) {
        if (current_task_ptr->pending_signals & (1ULL << i)) {
            // Check if signal is blocked (blocked_signals is 0-indexed, pending_signals is 1-indexed)
            if ((current_task_ptr->blocked_signals & (1ULL << (i - 1))) && i != 9 /*SIGKILL*/ && i != 19 /*SIGSTOP*/) {
                continue;
            }

            uint64_t *sa = &current_task_ptr->sigactions[i * 4];
            uint64_t handler = sa[0];
            uint64_t flags = sa[1];
            uint64_t restorer = sa[2];

            if (handler == (uint64_t)SIG_DFL) {
                // Default action depends on signal
                if (i == SIGSTOP || i == SIGTSTP || i == SIGTTIN || i == SIGTTOU) {
                    // Default action: stop the process
                    current_task_ptr->state = TASK_STOPPED;
                    current_task_ptr->stopped_by_signal = 1;
                    current_task_ptr->stop_reported = 0;
                    current_task_ptr->pending_signals &= ~(1ULL << i);
                    // Notify parent so waitpid(WUNTRACED) wakes up
                    for (int _j = 0; _j < MAX_TASKS; _j++) {
                        if (tasks[_j]->state != TASK_DEAD && tasks[_j]->pid == current_task_ptr->ppid) {
                            tasks[_j]->pending_signals |= (1ULL << SIGCHLD);
                            break;
                        }
                    }
                    // Yield to scheduler. syscall_entry holds sched_lock, and
                    // isr32 skips schedule() while sched_lock is held, so we
                    // must release/reacquire it around the yield for the stop
                    // to actually switch to another task (otherwise bash's
                    // tcgetpgrp/kill(0, SIGTTIN) loop spins forever).
                    if (sched_locked) spin_unlock(&sched_lock);
                    yield_sched();
                    if (sched_locked) spin_lock(&sched_lock);
                    continue;
                } else if (i == SIGCONT) {
                    // Default action: continue (already running, just clear)
                    current_task_ptr->pending_signals &= ~(1ULL << i);
                    continue;
                } else if (i == SIGCHLD || i == SIGURG || i == SIGWINCH) {
                    // Default action: ignore
                    current_task_ptr->pending_signals &= ~(1ULL << i);
                    continue;
                }
                // Default action: terminate
                current_task_ptr->pending_signals = 0;
                current_task_ptr->term_sig = i; // Ensure shell knows it was killed by signal
                exit_task(128 + i);
            } else if (handler == (uint64_t)SIG_IGN) {
                current_task_ptr->pending_signals &= ~(1ULL << i);
                continue;
            }

            current_task_ptr->pending_signals &= ~(1ULL << i);

            if (from_syscall && (flags & SA_RESTART) && frame->rax == (uint64_t)-EINTR && current_task_ptr->orig_rax != __NR_nanosleep && current_task_ptr->orig_rax != __NR_clock_nanosleep) {
                frame->rax = current_task_ptr->orig_rax;
                frame->rip -= 2; // Rewind the syscall instruction (2-byte `syscall`)
            }

            signal_stack_frame_t saved = { .context = *frame, .blocked_signals = current_task_ptr->blocked_signals };
            uint64_t delivery_size = 128 + sizeof(saved) + 16 + 8 + 15;
            if (frame->rsp < delivery_size) {
                current_task_ptr->pending_signals = 0;
                current_task_ptr->term_sig = SIGSEGV;
                exit_task(128 + SIGSEGV);
                return;
            }
            uint64_t user_rsp = frame->rsp - 128; // red zone
            user_rsp -= sizeof(saved);
            user_rsp &= ~15ULL;

            uint64_t sf_addr = user_rsp;
            uint64_t delivery_start = sf_addr - 16 - 8;
            if (!user_write_range_ok(current_task_ptr->ctx, delivery_start, sf_addr + sizeof(saved) - delivery_start)) {
                current_task_ptr->pending_signals = 0;
                current_task_ptr->term_sig = SIGSEGV;
                exit_task(128 + SIGSEGV);
                return;
            }
            (void)write_vmm(current_task_ptr->ctx, sf_addr, &saved, sizeof(saved));

            user_rsp -= 16; // always allocate space for siginfo
            uint32_t sinfo[4] = {i, 0, 0, 0};
            (void)write_vmm(current_task_ptr->ctx, user_rsp, &sinfo, sizeof(sinfo));
            uint64_t sinfo_addr = user_rsp;

            user_rsp -= 8;
            (void)write_vmm(current_task_ptr->ctx, user_rsp, &restorer, sizeof(uint64_t));

            frame->rip = handler;
            frame->rdi = i;
            frame->rsi = (flags & 4) ? sinfo_addr : 0;
            frame->rdx = 0;
            frame->rsp = user_rsp;

            current_task_ptr->blocked_signals |= sa[3];
            if (!(flags & SA_NODEFER)) current_task_ptr->blocked_signals |= 1ULL << (i - 1);
            current_task_ptr->blocked_signals &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            if (flags & SA_RESETHAND) sa[0] = (uint64_t)SIG_DFL;

            break;
        }
    }
}

void check_signals(syscall_frame_t *frame) {
    check_signals_context(frame, true, true);
}

void check_signals_from_user_exception(syscall_frame_t *frame) {
    check_signals_context(frame, false, false);
}

void check_signals_from_interrupt(interrupt_frame_t *irq) {
    if (!irq || (irq->cs & 3) != 3) return;

    syscall_frame_t frame = {
        .rax = irq->rax, .rbx = irq->rbx, .rcx = irq->rcx, .rdx = irq->rdx, .rsi = irq->rsi, .rdi = irq->rdi, .rsp = irq->rsp, .rbp = irq->rbp, .r8 = irq->r8, .r9 = irq->r9, .r10 = irq->r10, .r11 = irq->r11, .r12 = irq->r12, .r13 = irq->r13, .r14 = irq->r14, .r15 = irq->r15, .rip = irq->rip, .rflags = irq->rflags, };

    check_signals_context(&frame, false, false);

    irq->rax = frame.rax; irq->rbx = frame.rbx; irq->rcx = frame.rcx;
    irq->rdx = frame.rdx; irq->rsi = frame.rsi; irq->rdi = frame.rdi;
    irq->rsp = frame.rsp; irq->rbp = frame.rbp;
    irq->r8 = frame.r8; irq->r9 = frame.r9; irq->r10 = frame.r10;
    irq->r11 = frame.r11; irq->r12 = frame.r12; irq->r13 = frame.r13;
    irq->r14 = frame.r14; irq->r15 = frame.r15;
    irq->rip = frame.rip; irq->rflags = frame.rflags;
}

void check_futex_timeouts(void) {
    uint64_t now = time_get_realtime_us();

    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].state != FW_WAITING) continue;

        int idx = futex_waiters[i].task_idx;
        if (idx < 0 || idx >= MAX_TASKS) continue;

        if (futex_waiters[i].deadline_us != 0 && now >= futex_waiters[i].deadline_us) {
            futex_waiters[i].state = FW_TIMED_OUT;
            if (tasks[idx]->state == TASK_STOPPED)
                tasks[idx]->state = TASK_READY;
            continue;
        }

        if (tasks[idx]->pending_signals && tasks[idx]->state == TASK_STOPPED) {
            futex_waiters[i].state = FW_WOKEN;
            tasks[idx]->state = TASK_READY;
        }
    }
}

void wake_clear_child_tid(task_t *task) {
    if (task->clear_child_tid && task->ctx && user_write_range_ok(task->ctx, (uint64_t)task->clear_child_tid, sizeof(int))) {
        int zero = 0;
        (void)write_vmm(task->ctx, (uint64_t)task->clear_child_tid, &zero, sizeof(int));
        uint64_t phys = get_vmm_phys(task->ctx, (uint64_t)task->clear_child_tid);
        if (phys) {
            wake_futex(phys, 1, 0xFFFFFFFFU);
        }
    }
}

static void handle_futex_death(task_t *task, uint64_t uaddr, pid_t tid, bool pending_op) {
    if (!user_write_range_ok(task->ctx, (uint64_t)uaddr, sizeof(uint32_t))) return;

    uint64_t irq;
    spin_lock_irqsave(&futex_lock, &irq);

    uint32_t uval = 0;
    if (read_vmm(task->ctx, &uval, uaddr, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq); return; }

    // Pending-op unlock race: holder already cleared the TID field in
    // userspace but died before issuing FUTEX_WAKE.  Value is consistent, // just wake any waiter without touching it.
    if (pending_op && (uval & FUTEX_TID_MASK) == 0) {
        uint64_t phys = get_vmm_phys(task->ctx, uaddr);
        if (phys) wake_futex(phys, 1, FUTEX_BITSET_MATCH_ANY);
        spin_unlock_irqrestore(&futex_lock, irq);
        return;
    }

    // Only act if we are the recorded owner.
    if ((uval & FUTEX_TID_MASK) != (uint32_t)tid) {
        spin_unlock_irqrestore(&futex_lock, irq);
        return;
    }

    // Preserve FUTEX_WAITERS, set OWNER_DIED (clears the TID field).
    uint32_t mval = (uval & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
    (void)write_vmm(task->ctx, uaddr, &mval, sizeof(uint32_t));

    // If anyone was waiting, wake one so it observes EOWNERDEAD and either
    // recovers the mutex or marks it ENOTRECOVERABLE.
    if (uval & FUTEX_WAITERS) {
        uint64_t phys = get_vmm_phys(task->ctx, uaddr);
        if (phys) wake_futex(phys, 1, FUTEX_BITSET_MATCH_ANY);
    }

    spin_unlock_irqrestore(&futex_lock, irq);
}

void stat_to_statx(struct statx *sx, const struct stat *kst, const char *abs_path) {
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask      = STATX_BASIC_STATS;
    sx->stx_blksize   = kst->st_blksize ? (uint32_t)kst->st_blksize : 4096;
    sx->stx_attributes = 0;
    sx->stx_nlink     = kst->st_nlink;
    sx->stx_uid       = kst->st_uid;
    sx->stx_gid       = kst->st_gid;
    sx->stx_mode      = kst->st_mode;
    sx->stx_ino       = kst->st_ino;
    sx->stx_size      = (uint64_t)kst->st_size;
    sx->stx_blocks    = (uint64_t)kst->st_blocks;
    sx->stx_rdev_major = major(kst->st_rdev);
    sx->stx_rdev_minor = minor(kst->st_rdev);
    sx->stx_dev_major  = major(kst->st_dev);
    sx->stx_dev_minor  = minor(kst->st_dev);
    (void)abs_path;
    sx->stx_atime.tv_sec  = kst->st_atime;
    sx->stx_mtime.tv_sec  = kst->st_mtime;
    sx->stx_ctime.tv_sec  = kst->st_ctime;
    sx->stx_atime.tv_nsec = (uint32_t)kst->st_atim.tv_nsec;
    sx->stx_mtime.tv_nsec = (uint32_t)kst->st_mtim.tv_nsec;
    sx->stx_ctime.tv_nsec = (uint32_t)kst->st_ctim.tv_nsec;
}

static void statx_add_mount(struct statx *sx, const char *path, unsigned int mask) {
    if (!path) return;
    bool mount_root;
    uint64_t id = get_vfs_id(path, &mount_root);
    sx->stx_attributes_mask |= STATX_ATTR_MOUNT_ROOT;
    if (mount_root) sx->stx_attributes |= STATX_ATTR_MOUNT_ROOT;
    if (mask & STATX_MNT_ID_UNIQUE) {
        sx->stx_mnt_id = id;
        sx->stx_mask |= STATX_MNT_ID_UNIQUE;
    } else if (mask & STATX_MNT_ID) {
        sx->stx_mnt_id = id;
        sx->stx_mask |= STATX_MNT_ID;
    }
}

void statx_add_fs_metadata(struct statx *sx, const char *path, bool follow, unsigned int mask) {
    if (!path) return;
    if (check_ext4_path(path)) statx_ext4_metadata(path, sx, follow);
    if (mask & STATX_BTIME) {
        tmpfs_file_t file = follow ? stat_tmpfs(path) : stat_tmpfs_nofollow(path);
        if (file.mode) {
            sx->stx_btime.tv_sec = file.btime.tv_sec;
            sx->stx_btime.tv_nsec = file.btime.tv_nsec;
            sx->stx_mask |= STATX_BTIME;
        }
        else if (!check_ext4_path(path) && !check_iso9660_path(path) && !is_procfs_path(path) && !is_devtmpfs_path(path, NULL) && !is_devpts_path(path, NULL)) {
            initrd_file_t initrd_file = follow ? stat_initrd(path) : stat_initrd_nofollow(path);
            if (initrd_file.mode) {
                sx->stx_btime.tv_sec = initrd_file.btime.tv_sec;
                sx->stx_btime.tv_nsec = initrd_file.btime.tv_nsec;
                sx->stx_mask |= STATX_BTIME;
            }
        }
    }
    statx_add_mount(sx, path, mask);
}

int stat_fd_to_kst(int fd, struct stat *kst) {
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) return -EBADF;
    if (entry->type == FD_DEV && stat_virtual_device(entry->path, kst)) return 0;
    if (entry->type == FD_STREAM && stat_virtual_device("/dev/tty1", kst)) return 0;
    if (entry->type == FD_FILE) return stat_initrd_to_kst(entry->path, kst, true) ? 0 : -ENOENT;
    if (entry->type == FD_TMPFS) return stat_tmpfs_to_kst(entry->path, kst, true) ? 0 : -ENOENT;
    if (entry->type == FD_EXT4) return stat_ext4(entry->path, kst, true);
    if (entry->type == FD_ISO9660) return stat_iso9660(entry->path, kst, true);
    if (entry->type == FD_PROC) return stat_proc(entry->path, NULL, kst, true) ? 0 : -ENOENT;
    memset(kst, 0, sizeof(*kst));
    if (entry->type == FD_PIPE) kst->st_mode = S_IFIFO | 0600;
    else if (entry->type == FD_SOCKET) kst->st_mode = S_IFSOCK | 0777;
    else if (entry->type == FD_PTY_MASTER) kst->st_mode = S_IFCHR | 0600;
    else if (entry->type == FD_EPOLL || entry->type == FD_EPOLL_H) kst->st_mode = S_IFREG | 0600;
    else return -EBADF;
    kst->st_uid = current_task_ptr->euid;
    kst->st_gid = current_task_ptr->egid;
    kst->st_nlink = 1;
    kst->st_blksize = 4096;
    kst->st_ino = (ino_t)(uintptr_t)(entry->handle ? entry->handle : entry);
    stat_set_synthetic_times(kst);
    return 0;
}

void rollback_mmap(void *ptr, uint64_t num_pages, uint64_t retained_pages) {
    if (!ptr) return;
    uint64_t start = (uint64_t)ptr;
    for (uint64_t i = 0; i < num_pages; i++) unmap_vmm(current_task_ptr->ctx, start + i * PAGE_SIZE);
    remove_vma(&current_task_ptr->ctx->vmas, start, start + num_pages * PAGE_SIZE);
    current_task_ptr->ctx->mmap_pages = retained_pages;
}

flock_obj_t *find_advisory_lock_conflict(const fd_entry_t *entry, int requested_type, bool process_lock) {
    for (int i = 0; i < 128; i++) {
        flock_obj_t *obj = &global_flocks[i];
        if (!obj->used || obj->lock_type == 0 || strcmp(obj->path, entry->path) != 0) continue;
        if (process_lock && obj->owner_pid == current_task_ptr->pid) continue;
        if (!process_lock && obj == (flock_obj_t *)entry->handle) continue;
        if (requested_type == LOCK_EX || obj->lock_type == LOCK_EX) return obj;
    }
    return NULL;
}

int set_advisory_lock(fd_entry_t *entry, int lock_type, bool nonblocking, bool process_lock) {
    if (lock_type == 0) {
        if (entry->handle) {
            flock_obj_t *obj = (flock_obj_t *)entry->handle;
            if (!process_lock || obj->owner_pid == current_task_ptr->pid) {
                obj->lock_type = 0;
                obj->owner_pid = 0;
            }
        }
        return 0;
    }

    while (find_advisory_lock_conflict(entry, lock_type, process_lock)) {
        if (nonblocking) return -EAGAIN;
        // Avoid deadlock: flock is called with sched_lock held, so sleeping
        // with a raw delay would hold the spinlock. Yield properly instead.
        spin_unlock(&sched_lock);
        let_current_task_sleep(10000);
        spin_lock(&sched_lock);
        if (signal_pending()) return -EINTR;
    }

    if (!entry->handle) {
        entry->handle = alloc_flock_obj(entry->path);
        if (!entry->handle) return -ENOLCK;
    }
    flock_obj_t *obj = (flock_obj_t *)entry->handle;
    obj->lock_type = lock_type;
    obj->owner_pid = process_lock ? current_task_ptr->pid : 0;
    return 0;
}

int do_truncate_path(const char *abs_path, uint64_t length) {
    if (check_ext4_path(abs_path)) return -EROFS;
    if (check_iso9660_path(abs_path)) return -EROFS;
    if (check_vfat_path(abs_path)) {
        struct stat st;
        int status = stat_vfat(abs_path, &st, true);
        if (status < 0) return status;
        if (S_ISDIR(st.st_mode)) return -EISDIR;
        if (!can_access_stat_mode(&st, 0, 1, 0)) return -EACCES;
        return truncate_vfat(abs_path, length);
    }
    // tmpfs
    if (is_tmpfs_dir(abs_path)) {
        tmpfs_file_t f = stat_tmpfs(abs_path);
        if (!f.mode) return -ENOENT;
        if (!S_ISREG(f.mode)) return -EISDIR;
        struct stat st;
        if (!stat_tmpfs_to_kst(abs_path, &st, true) || !can_access_stat_mode(&st, 0, 1, 0)) return -EACCES;
        return truncate_tmpfs(abs_path, length);
    }
    // initrd / overlay
    initrd_file_t f = read_initrd(abs_path);
    if (!f.mode) return -ENOENT;
    if (f.size && !f.data) return -EIO;
    if (!can_access_initrd(&f, 0, 1, 0)) return -EACCES;
    if (length > INITRD_MAX_FILE_SIZE) return -EFBIG;

    if (length == f.size) return 0;

    if (length < f.size) {
        // Shrink: write only the first 'length' bytes back.
        return write_initrd(abs_path, f.data, length, f.mode ? f.mode : 0644, f.uid, f.gid);
    } else {
        // Extend: write existing data + zero-pad to 'length'.
        uint64_t extra = length - f.size;
        void *newbuf = malloc(length);
        if (!newbuf) return -ENOMEM;
        if (f.size) memcpy(newbuf, f.data, f.size);
        memset((char *)newbuf + f.size, 0, extra);
        int r = write_initrd(abs_path, newbuf, length, f.mode ? f.mode : 0644, f.uid, f.gid);
        free(newbuf);
        return r;
    }
}

int change_working_directory(const char *abs_path) {
    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));

    int access = check_directory_access(resolved, false);
    if (access < 0 && !is_procfs_path(resolved)) return access;

    // procfs directories (e.g. /proc/<pid>, /proc/self, /proc/<pid>/fd) are
    // virtual: they have no initrd backing, so validate them through the
    // procfs resolver. Without this, `cd /proc/<pid>` fails with ENOENT even
    // though getdents on /proc lists the pid.
    if (is_procfs_path(resolved)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!resolve_procfs(resolved, self, &n)) return -ENOENT;
        if (!is_procfs_dir(&n)) return -ENOTDIR;
        strncpy(current_task_ptr->cwd, resolved, 255);
        current_task_ptr->cwd[255] = '\0';
        return 0;
    }

    // tmpfs directories: validate via tmpfs, then set cwd.
    if (is_tmpfs_dir(resolved)) {
        tmpfs_file_t dir = stat_tmpfs(resolved);
        if (!dir.mode) return -ENOENT;
        if (!S_ISDIR(dir.mode)) return -ENOTDIR;
        strncpy(current_task_ptr->cwd, resolved, 255);
        current_task_ptr->cwd[255] = '\0';
        return 0;
    }

    if (check_ext4_path(resolved)) {
        struct stat st;
        int status = stat_ext4(resolved, &st, true);
        if (status < 0) return status;
        if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
        if (!can_access_stat_mode(&st, 1, 0, 1)) return -EACCES;
        strncpy(current_task_ptr->cwd, resolved, 255);
        current_task_ptr->cwd[255] = '\0';
        return 0;
    }

    if (check_iso9660_path(resolved)) {
        struct stat st;
        int status = stat_iso9660(resolved, &st, true);
        if (status < 0) return status;
        if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
        if (!can_access_stat_mode(&st, 1, 0, 1)) return -EACCES;
        strncpy(current_task_ptr->cwd, resolved, 255);
        current_task_ptr->cwd[255] = '\0';
        return 0;
    }

    if (check_vfat_path(resolved)) {
        struct stat st;
        int status = stat_vfat(resolved, &st, true);
        if (status < 0) return status;
        if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
        if (!can_access_stat_mode(&st, 1, 0, 1)) return -EACCES;
        strncpy(current_task_ptr->cwd, resolved, 255);
        current_task_ptr->cwd[255] = '\0';
        return 0;
    }

    initrd_file_t dir = read_initrd(resolved);
    if (!dir.data && !dir.mode) return -ENOENT;
    if (!S_ISDIR(dir.mode) && strcmp(resolved, "/") != 0) return -ENOTDIR;

    strncpy(current_task_ptr->cwd, resolved, 255);
    current_task_ptr->cwd[255] = '\0';
    return 0;
}

static int check_path_access(const char *path, const char *abs_path, int mode, bool follow) {
    int want_read = (mode & R_OK) != 0;
    int want_write = (mode & W_OK) != 0;
    int want_exec = (mode & X_OK) != 0;

    if (want_write && (check_ext4_path(abs_path) || check_iso9660_path(abs_path))) return -EROFS;

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst) || stat_tmpfs_to_kst(abs_path, &kst, follow) || stat_ext4_to_kst(abs_path, &kst, follow) || stat_iso9660_to_kst(abs_path, &kst, follow) || stat_vfat_to_kst(abs_path, &kst, follow) || stat_proc(abs_path, path, &kst, follow)) {
        return mode == F_OK || can_access_stat_mode(&kst, want_read, want_write, want_exec) ? 0 : -EACCES;
    }

    initrd_file_t file = follow ? stat_initrd(abs_path) : stat_initrd_nofollow(abs_path);
    if (!file.mode) return -ENOENT;
    return mode == F_OK || can_access_initrd(&file, want_read, want_write, want_exec) ? 0 : -EACCES;
}

int check_access_at(int dirfd, const char *user_path, int mode, int flags) {
    if (!user_path) return -EFAULT;
    if (mode & ~(R_OK | W_OK | X_OK)) return -EINVAL;
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;

    char path[256];
    int status = copy_string_from_user(path, user_path, sizeof(path));
    if (status < 0) return status;
    if (!path[0]) return -ENOENT;

    char abs_path[256];
    status = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (status < 0) return status;
    return check_path_access(path, abs_path, mode, !(flags & AT_SYMLINK_NOFOLLOW));
}

void cleanup_futex_task(int task_idx) {
    uint64_t irq;
    spin_lock_irqsave(&futex_lock, &irq);
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].state == FW_WAITING && futex_waiters[i].task_idx == task_idx) {
            futex_waiters[i].state = FW_FREE;
        }
    }
    spin_unlock_irqrestore(&futex_lock, irq);
}

void process_robust_list(task_t *task) {
    if (!task->robust_list_head || !task->ctx) {
        task->robust_list_head = NULL;
        return;
    }

    uint64_t head = (uint64_t)task->robust_list_head;
    if (!user_range_ok(task->ctx, (uint64_t)head, 24)) {
        task->robust_list_head = NULL;
        return;
    }

    uint64_t hdr[3];
    if (read_vmm(task->ctx, hdr, head, 24) < 0) { task->robust_list_head = NULL; return; }

    // bit 0 of list pointers tags a PI futex; we don't implement PI, so mask it
    uint64_t first   = hdr[0] & ~1UL;
    int64_t  foff    = (int64_t)hdr[1];
    uint64_t pending = hdr[2] & ~1UL;

    pid_t tid = task->pid;

    uint64_t entry = first;
    int limit = 2048;  // ROBUST_LIST_LIMIT, defends against loops/junk
    while (entry && entry != head && limit-- > 0) {
        uint64_t next = 0;
        if (!user_range_ok(task->ctx, (uint64_t)entry, sizeof(uint64_t))) break;
        if (read_vmm(task->ctx, &next, entry, sizeof(uint64_t)) < 0) break;
        next &= ~1UL;

        if (entry != pending) {
            handle_futex_death(task, entry + foff, tid, false);
        }

        entry = next;
    }

    // The pending-op marker is for a lock whose userspace unlock just
    // finished; process it last as the Linux kernel does.
    if (pending && pending != head) {
        handle_futex_death(task, pending + foff, tid, true);
    }

    task->robust_list_head = NULL;
}
