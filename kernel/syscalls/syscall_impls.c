#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <fcntl.h>
#include <flock.h>
#include <dirent.h>
#include <time.h>
#include <times.h>
#include <wait.h>
#include <termios.h>
#include <limits.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <asm/prctl.h>
#include <linux/rseq.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/fb.h>
#include <sys/stat.h>
#include <sys/statx.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sys/futex.h>
#include <sys/reboot.h>
#include <sys/random.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/sysmacros.h>
#include <main/log.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/elf.h>
#include <main/halt.h>
#include <main/hostname.h>
#include <main/domainname.h>
#include <main/timekeeping.h>
#include <main/utsname.h>
#include <main/mp.h>
#include <main/msr.h>
#include <main/fd.h>
#include <main/sched.h>
#include <main/signal.h>
#include <main/string.h>
#include <main/rng.h>
#include <io/fb.h>
#include <io/fonts.h>
#include <io/devices.h>
#include <io/devtmpfs.h>
#include <io/devpts.h>
#include <io/pts_devices.h>
#include <io/initrd.h>
#include <io/terminal.h>
#include <io/keyboard.h>
#include <io/tty.h>
#include <io/pty.h>
#include <io/time.h>
#include <io/power.h>
#include <io/sockets.h>
#include <io/net.h>
#include <io/unix_sockets.h>
#include <io/serial.h>
#include <io/procfs.h>
#include <io/tmpfs.h>
#include <io/ext4.h>
#include <io/usb.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/vma.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>

#define RFLAGS_IOPL (3ULL << 12)
#define RFLAGS_NT (1ULL << 14)
#define RFLAGS_RF (1ULL << 16)
#define RFLAGS_VM (1ULL << 17)
#define RFLAGS_FIXED (1ULL << 1)
#define USER_RFLAGS_FORBIDDEN (RFLAGS_IOPL | RFLAGS_NT | RFLAGS_RF | RFLAGS_VM)

/*
   Tried to fucking modularize this...
   Didn't go well...
   To who is reading this: Please don't try to modularize this, it's too late...just keep adding on...
 */

static futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];

static spinlock_t stdin_lock = SPINLOCK_INIT;
static spinlock_t futex_lock = SPINLOCK_INIT;

static bool user_address_range_ok(uint64_t addr, uint64_t size) {
    if (addr >= USER_ADDR_MAX) return false;
    if (size > USER_ADDR_MAX - addr) return false;
    return true;
}

static bool user_range_ok(vmm_context_t *ctx, uint64_t addr, uint64_t size) {
    return user_address_range_ok(addr, size) && vmm_user_range_valid(ctx, addr, size, false);
}

static bool user_write_range_ok(vmm_context_t *ctx, uint64_t addr, uint64_t size) {
    return user_address_range_ok(addr, size) && vmm_user_range_valid(ctx, addr, size, true);
}

static bool user_page_range_ok(uint64_t addr, uint64_t size, uint64_t *start, uint64_t *end) {
    if (!start || !end || size == 0 || !user_address_range_ok(addr, size)) return false;

    uint64_t last = addr + size - 1;
    uint64_t page_start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t page_end = (last & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;
    if (page_end < page_start || page_end > USER_ADDR_MAX) return false;

    *start = page_start;
    *end = page_end;
    return true;
}

static bool fd_allows_read(const fd_entry_t *entry) {
    return entry && (entry->flags & O_ACCMODE) != O_WRONLY;
}

static bool fd_allows_write(const fd_entry_t *entry) {
    int access_mode = entry ? (int)(entry->flags & O_ACCMODE) : O_RDONLY;
    return entry && (access_mode == O_WRONLY || access_mode == O_RDWR);
}

static mode_t apply_current_umask(mode_t mode) {
    mode_t mask = current_task_ptr ? current_task_ptr->umask : 0022;
    return (mode & 07777) & ~mask;
}

static bool user_range_is_mapped(vmm_context_t *ctx, uint64_t addr, uint64_t size) {
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

static bool can_access_initrd(const initrd_file_t *file, int want_read, int want_write, int want_exec) {
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

static bool can_access_stat_mode(const struct stat *st, int want_read, int want_write, int want_exec) {
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

static int proc_self_idx(void) {
    if (!current_task_ptr) return -1;
    return current_task;  // current_task is the running task's index
}

static void resolve_path_symlinks_ex(const char *path, char *out, size_t out_size, bool follow_final) {
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
static void resolve_path_symlinks(const char *path, char *out, size_t out_size) { resolve_path_symlinks_ex(path, out, out_size, true); }

static int build_abs_path_at(int dirfd, const char *path, char *out, size_t out_size) {
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

static void build_abs_path(const char *path, char *out, size_t out_size) { build_abs_path_at(AT_FDCWD, path, out, out_size); }

static int copy_string_from_user(char *dest, const char *src, size_t capacity) {
    if (!dest || !src || capacity == 0) return -EFAULT;
    for (size_t i = 0; i < capacity; i++) {
        if (copy_from_user(&dest[i], &src[i], 1) < 0) return -EFAULT;
        if (dest[i] == '\0') return 0;
    }
    dest[capacity - 1] = '\0';
    return -ENAMETOOLONG;
}

static int copy_from_user_strarray(char ***out_karray, const char **user_arr, size_t max_elements) {
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

static int ptm_path_idx(const char *path) {
    if (path[0]!='p'||path[1]!='t'||path[2]!='m'||path[3]!=':') return -1;
    const char *n = path + 4;
    int idx = 0;
    while (*n >= '0' && *n <= '9') idx = idx * 10 + (*n++ - '0');
    return idx;
}

static void free_strarray(char **arr, int count) { if (!arr) return; for (int i = 0; i < count; i++) free(arr[i]); free(arr); }

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

static bool stat_virtual_device(const char *abs_path, struct stat *kst) {
    // build_abs_path_at already resolved intermediate symlinks;
    // resolve the final component too for virtual device lookup.
    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));

    char rel_path[256];
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs), // so devtmpfs would incorrectly match /dev/pts paths with rel="pts".
    if (match_vfs_path(resolved, "devpts", rel_path)) {
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
    if (match_vfs_path(resolved, "devtmpfs", rel_path)) {
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
static bool stat_tmpfs_to_kst(const char *abs_path, struct stat *kst, bool follow) {
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

static bool stat_initrd_to_kst(const char *abs_path, struct stat *kst, bool follow) {
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

static bool stat_ext4_to_kst(const char *abs_path, struct stat *kst, bool follow) {
    if (!check_ext4_path(abs_path)) return false;
    return stat_ext4(abs_path, kst, follow) == 0;
}

static int check_directory_access(const char *path, bool write) {
    struct stat st;
    if (check_ext4_path(path)) {
        if (!stat_ext4_to_kst(path, &st, true)) return -ENOENT;
        if (write) return -EROFS;
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

static int check_parent_access(const char *path, bool modify) {
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

static bool stat_proc(const char *abs_path, const char *orig_path, struct stat *kst, bool follow_self) {
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

static int proc_open_common(char *abs_path, size_t abs_size, uint32_t flags) {
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
static int open_tmpfs_common(const char *abs_path, uint32_t flags, mode_t mode) {
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
static int open_ext4_common(const char *abs_path, uint32_t flags) {
    if (!check_ext4_path(abs_path)) return 1;

    struct stat st;
    int status = stat_ext4(abs_path, &st, true);
    if (status < 0) return status;

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    if (want_write || (flags & (O_CREAT | O_TRUNC))) return -EROFS;
    if ((flags & O_DIRECTORY) && !S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (!can_access_stat_mode(&st, 1, 0, S_ISDIR(st.st_mode))) return -EACCES;
    return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_EXT4, flags);
}

static uint64_t resolve_futex_key(uint32_t *uaddr, syscall_frame_t *frame) {
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

static void wait_futex(syscall_frame_t *frame, uint64_t phys, uint32_t val, struct timespec *timeout_ptr, uint32_t bitset, bool absolute_timeout) {
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

static int wake_futex(uint64_t phys, uint32_t max_wake, uint32_t bitset) {
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

static int fill_rlimit(int resource, rlimit_t *lim) {
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

static uint16_t emit_dirent64(uint64_t bufp, uint64_t *written, uint64_t buflen, uint64_t ino, uint64_t off, uint8_t type, const char *name) {
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

static uint16_t emit_dirent(uint64_t bufp, uint64_t *written, uint64_t buflen, uint64_t ino, uint64_t off, uint8_t type, const char *name) {
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

static void resolve_dir_for_readdir(const char *fd_path, char *prefix_out, size_t prefix_size, char *abs_out, size_t abs_size) {
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



static int tty_rel_to_idx(const char *rel) {
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

static int pty_rel_to_idx(const char *rel) {
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

static int ioctl_tty_idx(fd_entry_t *entry) {
    if (current_task_ptr->ctty_idx >= 0) return current_task_ptr->ctty_idx;

    if (entry && (entry->type == FD_DEV || entry->type == FD_STREAM)) {
        char rel[256];
        if (entry->type == FD_STREAM) {
            return 0;
        }
        if (match_vfs_path(entry->path, "devtmpfs", rel)) {
            int idx = tty_rel_to_idx(rel);
            if (idx >= 0) return idx;
            if (strncmp(rel, "pts/", 4) == 0) {
                idx = pty_rel_to_idx(rel + 4);
                if (idx >= 0) return 100 + idx;
            }
        } else if (match_vfs_path(entry->path, "devpts", rel)) {
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
                // at least one byte — return what we have (VMIN=1, VTIME=0
                // semantics by default).
                break;
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
        if (match_vfs_path(entry->path, "devtmpfs", rel)) {
            tty_idx = tty_rel_to_idx(rel);
        } else if (match_vfs_path(entry->path, "devpts", rel)) {
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

static int64_t do_select(int nfds, uint8_t *k_read, uint8_t *k_write, uint8_t *k_except, uint8_t *out_read, uint8_t *out_write, uint8_t *out_except, int out_bytes, int64_t timeout_us) {
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

static int do_clock_nanosleep(int clock_id, int flags, const struct timespec *req, struct timespec *rem) {
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

    while (sleep_clock_now_ns(wait_clock) < deadline) {
        if (signal_pending()) {
            if (!absolute && rem) {
                ktime_t now = sleep_clock_now_ns(wait_clock);
                struct timespec remaining = sleep_ns_to_timespec(now < deadline ? deadline - now : 0);
                if (copy_to_user(rem, &remaining, sizeof(remaining)) < 0) return -EFAULT;
            }
            return -EINTR;
        }

        ktime_t remaining_ns = deadline - sleep_clock_now_ns(wait_clock);
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

static task_t *find_priority_task(int which, id_t who) {
    if (which != PRIO_PROCESS) return NULL;
    if (who == 0 || who == (id_t)current_task_ptr->pid) return current_task_ptr;
    return task_by_pid((pid_t)who);
}

static int prepare_unix_socket_path(uint8_t *addr, uint32_t *addrlen, bool binding) {
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


static int reject_procfs_mutation(const char *path) {
    if (check_ext4_path(path)) return -EROFS;
    if (is_procfs_path(path)) return -EROFS;
    return 0;
}

static int reject_virtual_removal(const char *path) {
    int status = reject_procfs_mutation(path);
    if (status < 0) return status;
    if (match_vfs_path(path, "devtmpfs", NULL) || match_vfs_path(path, "devpts", NULL)) return -EPERM;
    return 0;
}

static int change_path_ownership(const char *path, uid_t uid, gid_t gid, bool follow) {
    if (current_task_ptr->euid != 0) return -EPERM;
    if (check_ext4_path(path)) return -EROFS;
    if (match_vfs_path(path, "devtmpfs", NULL) || match_vfs_path(path, "devpts", NULL) || is_procfs_path(path)) return -EPERM;
    if (is_tmpfs_dir(path)) return chown_tmpfs(path, uid, gid, follow);
    return chown_initrd(path, uid, gid, follow);
}


static int set_path_times(const char *path, struct timespec atime, bool set_atime, struct timespec mtime, bool set_mtime, bool follow) {
    struct stat st = {0};
    bool is_virtual = stat_virtual_device(path, &st);
    bool is_tmpfs = !is_virtual && stat_tmpfs_to_kst(path, &st, follow);
    bool is_ext4 = !is_virtual && !is_tmpfs && stat_ext4_to_kst(path, &st, follow);
    bool is_proc = !is_virtual && !is_tmpfs && !is_ext4 && stat_proc(path, path, &st, follow);
    bool is_initrd = !is_virtual && !is_tmpfs && !is_ext4 && !is_proc && stat_initrd_to_kst(path, &st, follow);
    if (!is_virtual && !is_tmpfs && !is_ext4 && !is_proc && !is_initrd) return -ENOENT;
    if (!set_atime && !set_mtime) return 0;

    bool owner = current_task_ptr->fsuid == 0 || current_task_ptr->fsuid == st.st_uid;
    if (!owner && !can_access_stat_mode(&st, 0, 1, 0)) return -EACCES;
    if (is_ext4 || is_virtual || is_proc) return -EROFS;
    if (is_tmpfs) return set_tmpfs_times(path, atime, set_atime, mtime, set_mtime, follow);
    return set_initrd_times(path, atime, set_atime, mtime, set_mtime, follow);
}

static int get_utimens_times(const struct timespec *user_times, struct timespec *atime, bool *set_atime, struct timespec *mtime, bool *set_mtime) {
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
            if (match_vfs_path(entry->path, "devtmpfs", rel)) {
                tty_idx = tty_rel_to_idx(rel);
            } else if (match_vfs_path(entry->path, "devpts", rel)) {
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
static int epoll_collect(epoll_instance_t *epi, struct epoll_event *out, int maxevents) {
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

static int epoll_find_interest(epoll_instance_t *epi, int fd) {
    for (int i = 0; i < epi->count; i++) {
        if (epi->interests[i].watched_fd == fd) return i;
    }
    return -1;
}

static int do_epoll_create1(int flags) {
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

static int do_epoll_wait(syscall_frame_t *frame, int64_t timeout_us, uint64_t sigmask_arg) {
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

static int timeval_to_us(const struct timeval *tv, uint64_t *out) {
    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return -EINVAL;
    if ((uint64_t)tv->tv_sec > (UINT64_MAX - (uint64_t)tv->tv_usec) / 1000000ULL) return -EINVAL;
    *out = (uint64_t)tv->tv_sec * 1000000ULL + (uint64_t)tv->tv_usec;
    return 0;
}

static int timespec_to_us(const struct timespec *ts, int64_t *out) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) return -EINVAL;
    if ((uint64_t)ts->tv_sec > ((uint64_t)INT64_MAX - (uint64_t)ts->tv_nsec / 1000ULL) / 1000000ULL) return -EINVAL;
    *out = (int64_t)((uint64_t)ts->tv_sec * 1000000ULL + (uint64_t)ts->tv_nsec / 1000ULL);
    return 0;
}

static void fill_real_itimer(task_t *task, struct itimerval *value) {
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

static void stat_to_statx(struct statx *sx, const struct stat *kst, const char *abs_path) {
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

static void statx_add_fs_metadata(struct statx *sx, const char *path, bool follow, unsigned int mask) {
    if (!path) return;
    if (check_ext4_path(path)) statx_ext4_metadata(path, sx, follow);
    if (mask & STATX_BTIME) {
        tmpfs_file_t file = follow ? stat_tmpfs(path) : stat_tmpfs_nofollow(path);
        if (file.mode) {
            sx->stx_btime.tv_sec = file.btime.tv_sec;
            sx->stx_btime.tv_nsec = file.btime.tv_nsec;
            sx->stx_mask |= STATX_BTIME;
        } else if (!check_ext4_path(path) && !is_procfs_path(path) && !match_vfs_path(path, "devtmpfs", NULL) && !match_vfs_path(path, "devpts", NULL)) {
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

static int stat_fd_to_kst(int fd, struct stat *kst) {
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) return -EBADF;
    if (entry->type == FD_DEV && stat_virtual_device(entry->path, kst)) return 0;
    if (entry->type == FD_STREAM && stat_virtual_device("/dev/tty1", kst)) return 0;
    if (entry->type == FD_FILE) return stat_initrd_to_kst(entry->path, kst, true) ? 0 : -ENOENT;
    if (entry->type == FD_TMPFS) return stat_tmpfs_to_kst(entry->path, kst, true) ? 0 : -ENOENT;
    if (entry->type == FD_EXT4) return stat_ext4(entry->path, kst, true);
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

void sys_read(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint8_t *buf = (uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    // Validate user buffer
    if (count > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (!fd_allows_read(entry)) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_STREAM) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
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
        frame->rax = (uint64_t)got;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (match_vfs_path(entry->path, "devtmpfs", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }

            int tty_idx = tty_rel_to_idx(rel);
            if (tty_idx >= 0) {
                char local_buf[4096];
                uint8_t *kbuf = (count <= sizeof(local_buf)) ? (uint8_t*)local_buf : malloc(count);
                if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
                int64_t got = read_dev_tty((char *)kbuf, count, tty_idx);
                if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
                if (kbuf != (uint8_t*)local_buf) free(kbuf);
                frame->rax = (uint64_t)got;
                return;
            }

            char local_buf[4096];
            uint8_t *kbuf = (count <= sizeof(local_buf)) ? (uint8_t*)local_buf : malloc(count);
            if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
            res = read_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, res) < 0) { res = (uint64_t)-EFAULT; } else if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != (uint8_t*)local_buf) free(kbuf);
        } else if (match_vfs_path(entry->path, "devpts", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
            char local_buf[4096];
            uint8_t *kbuf = (count <= sizeof(local_buf)) ? (uint8_t*)local_buf : malloc(count);
            if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
            res = read_pts_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, res) < 0) { res = (uint64_t)-EFAULT; } else if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != (uint8_t*)local_buf) free(kbuf);
        } else {
            frame->rax = (uint64_t)-ENODEV; return;
        }
        frame->rax = res;
        return;
    }

    if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int got = read_pty_master(idx, (char *)kbuf, (int)count);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got) < 0) got = -EFAULT;
        free(kbuf);
        frame->rax = (got < 0) ? (uint64_t)-EIO : (uint64_t)got;
        return;
    }

    if (entry->type == FD_PIPE) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t got = read_unix_handle((unix_handle_t *)entry->handle, kbuf, count, entry->flags);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got) < 0) got = -EFAULT;
        free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }

    if (entry->type == FD_SOCKET) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        socket_t *sock = (socket_t *)entry->handle;
        int64_t got = -EBADF;
        if (sock && sock->ops && sock->ops->read) {
            got = sock->ops->read(sock, kbuf, count, entry->flags);
        }
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got) < 0) got = -EFAULT;
        free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }

    if (entry->type == FD_PROC || entry->type == FD_TMPFS || entry->type == FD_EXT4 || entry->type == FD_FILE) {
        if (count == 0) { frame->rax = 0; return; }
        if (count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        char local_buf[4096];
        uint8_t *kbuf = count <= sizeof(local_buf) ? (uint8_t *)local_buf : malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t got = read_vfs(entry->path, kbuf, count, entry->offset);
        if (got >= 0) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
            else entry->offset += (uint64_t)got;
        }
        if (kbuf != (uint8_t *)local_buf) free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }

    frame->rax = (uint64_t)-EBADF;
}

void sys_write(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int tty_idx;
    const uint8_t *buf = (const uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    // Validate user buffer pointer
    if (!buf) { frame->rax = (uint64_t)-EINVAL; return; }
    if (count > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (!fd_allows_write(entry)) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_EXT4) { frame->rax = (uint64_t)-EROFS; return; }

    uint8_t local_buf[4096];

    if (entry->type == FD_STREAM) {
        tty_idx = current_task_ptr->ctty_idx >= 0 && current_task_ptr->ctty_idx < NUM_TTYS ? current_task_ptr->ctty_idx : keyboard_tty;
        uint64_t processed = 0;
        while (processed < count) {
            poll_usb_hcds();
            if (signal_pending()) break;
            uint64_t chunk = count - processed;
            if (chunk > sizeof(local_buf)) chunk = sizeof(local_buf);
            if (read_vmm(current_task_ptr->ctx, local_buf, (uint64_t)buf + processed, chunk) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            write_terminal_tty(tty_idx, (const char *)local_buf, chunk, false);
            processed += chunk;
        }
        frame->rax = !processed && signal_pending() ? (uint64_t)-EINTR : processed;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (match_vfs_path(entry->path, "devtmpfs", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
            uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
            if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
            if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
            res = write_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != local_buf) free(kbuf);
        } else if (match_vfs_path(entry->path, "devpts", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
            uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
            if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
            if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
            res = write_pts_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) entry->offset += res;
            if (kbuf != local_buf) free(kbuf);
        } else {
            frame->rax = (uint64_t)-ENODEV; return;
        }
        frame->rax = res;
        return;
    }

    if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
        int w = write_pty_master(idx, (const char *)kbuf, (int)count);
        if (kbuf != local_buf) free(kbuf);
        frame->rax = (w < 0) ? (uint64_t)-EIO : (uint64_t)w;
        return;
    }

    if (entry->type == FD_PIPE) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
        int64_t w = write_unix_handle((unix_handle_t *)entry->handle, kbuf, count, entry->flags);
        if (kbuf != local_buf) free(kbuf);
        frame->rax = (uint64_t)w;
        return;
    }

    if (entry->type == FD_SOCKET) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
        socket_t *sock = (socket_t *)entry->handle;
        int64_t w = -EBADF;
        if (sock && sock->ops && sock->ops->write) {
            w = sock->ops->write(sock, kbuf, count, entry->flags);
        }
        if (kbuf != local_buf) free(kbuf);
        frame->rax = (uint64_t)w;
        return;
    }

    if (entry->type == FD_TMPFS) {
        tmpfs_file_t tf = read_tmpfs(entry->path);

        // O_APPEND: write at current end of file
        if (entry->flags & O_APPEND) entry->offset = tf.size;

        uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }

        int res = write_tmpfs_partial(entry->path, kbuf, entry->offset, count, tf.mode ? tf.mode : 0644, tf.mode ? tf.uid : current_task_ptr->euid, tf.mode ? tf.gid : current_task_ptr->egid);
        if (kbuf != local_buf) free(kbuf);
        if (res < 0) { frame->rax = (uint64_t)res; return; }
        entry->offset += count;
        frame->rax = count;
        return;
    }

    // O_APPEND: every write goes to the current end of file
    if (entry->flags & O_APPEND) {
        initrd_file_t file = read_initrd(entry->path);
        entry->offset = file.size;
    }

    // Copy the new bytes into a small kernel buffer first, then hand them to
    // write_initrd_partial which reuses the file's existing overlay buffer
    // (growing it geometrically) instead of re-copying the whole file on
    // every write.  This turns O(file_size) per write into O(off+count) —
    // critical for tools like dd that issue many small writes to a growing
    // file (e.g. building a 1.5MB image in 512-byte chunks previously cost
    // ~4GB of redundant memcpy -> 35s).
    uint8_t *kbuf = count <= sizeof(local_buf) ? local_buf : malloc(count);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
    if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count) < 0) { if (kbuf != local_buf) free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }

    int res = write_initrd_partial(entry->path, kbuf, entry->offset, count);
    if (kbuf != local_buf) free(kbuf);

    if (res < 0) { frame->rax = (uint64_t)res; return; }
    entry->offset += count;
    frame->rax = count;
}

void sys_open(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    uint32_t flags = (uint32_t)frame->rsi;
    mode_t mode = (mode_t)frame->rdx;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path[256];
    int cr = copy_string_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));
    strncpy(abs_path, resolved, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';

    char rel_path[256];
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs), // so devtmpfs would incorrectly match /dev/pts paths with rel="pts/...".
    if (match_vfs_path(abs_path, "devpts", rel_path)) {
        if (strcmp(rel_path, "ptmx") == 0) {
            int idx = alloc_pty();
            if (idx < 0) { frame->rax = (uint64_t)-ENOSPC; return; }
            char ptm_path[32];
            ptm_path[0]='p'; ptm_path[1]='t'; ptm_path[2]='m'; ptm_path[3]=':';
            if (idx < 10) { ptm_path[4]='0'+idx; ptm_path[5]='\0'; }
            else          { ptm_path[4]='1'; ptm_path[5]='0'+(idx-10); ptm_path[6]='\0'; }
            int fd = alloc_fd(&current_task_ptr->fd_table, ptm_path, FD_PTY_MASTER, flags);
            if (fd < 0) { release_pty_master(idx); frame->rax = (uint64_t)fd; return; }
            frame->rax = (uint64_t)fd;
            return;
        }
        if (rel_path[0] != '\0' && !devpts_device_exists(rel_path)) {
            initrd_file_t file = read_initrd(abs_path);
            if (!S_ISDIR(file.mode)) {
                frame->rax = (uint64_t)-ENOENT;
                return;
            }
        }
        int pty_idx = 0;
        const char *p = rel_path;
        if (*p == '\0') {
            pty_idx = -1;
        } else {
            while (*p >= '0' && *p <= '9') { pty_idx = pty_idx * 10 + (*p - '0'); p++; }
            if (*p != '\0') pty_idx = -1;
        }
        if (pty_idx >= 0 && pty_idx < NUM_PTYS) { int r = open_pty_slave(pty_idx); if (r < 0) { frame->rax = (uint64_t)r; return; } }
        int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_DEV, flags);
        if (fd < 0 && pty_idx >= 0 && pty_idx < NUM_PTYS)
            release_pty_slave(pty_idx);
        frame->rax = (uint64_t)fd;
        return;
    } else if (match_vfs_path(abs_path, "devtmpfs", rel_path)) {
        if (rel_path[0] != '\0' && !device_exists_on_devtmpfs(rel_path)) {
            initrd_file_t file = read_initrd(abs_path);
            if (!S_ISDIR(file.mode)) {
                frame->rax = (uint64_t)-ENOENT;
                return;
            }
        }

        if (rel_path[0] != '\0') {
            struct stat dev_st = {0};
            if (get_device_mode(rel_path, &dev_st.st_mode) == 0) {
                int want_write = (flags & O_ACCMODE) != O_RDONLY;
                int want_read = (flags & O_ACCMODE) != O_WRONLY;
                if (!can_access_stat_mode(&dev_st, want_read, want_write, 0)) {
                    frame->rax = (uint64_t)-EACCES;
                    return;
                }
            }
        }

        if (strcmp(rel_path, "ptmx") == 0) {
            int idx = alloc_pty();
            if (idx < 0) { frame->rax = (uint64_t)-ENOSPC; return; }
            char ptm_path[32];
            ptm_path[0]='p'; ptm_path[1]='t'; ptm_path[2]='m'; ptm_path[3]=':';
            if (idx < 10) { ptm_path[4]='0'+idx; ptm_path[5]='\0'; }
            else          { ptm_path[4]='1'; ptm_path[5]='0'+(idx-10); ptm_path[6]='\0'; }
            int fd = alloc_fd(&current_task_ptr->fd_table, ptm_path, FD_PTY_MASTER, flags);
            if (fd < 0) { release_pty_master(idx); frame->rax = (uint64_t)fd; return; }
            set_keyboard_pty(idx);
            frame->rax = (uint64_t)fd;
            return;
        }

        int pty_idx = pty_slave_path_idx(rel_path);
        if (pty_idx >= 0) { int r = open_pty_slave(pty_idx); if (r < 0) { frame->rax = (uint64_t)r; return; } }
        int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_DEV, flags);
        if (fd < 0 && pty_idx >= 0) release_pty_slave(pty_idx);

        if (fd >= 0 && pty_idx < 0) {
            int open_tty = tty_rel_to_idx(rel_path);
            if (open_tty >= 0 && open_tty < NUM_TTYS) {
                tty_t *ft = get_tty(open_tty);
                if (ft) {
                    uint64_t irq;
                    spin_lock_irqsave(&tty_lock, &irq);
                    ft->input.head = ft->input.tail = 0;
                    spin_unlock_irqrestore(&tty_lock, irq);
                }
            }
        }

        if (fd >= 0 && current_task_ptr->ctty_idx < 0 && !(flags & O_NOCTTY) && current_task_ptr->pid == current_task_ptr->sid) {
            int tidx = -1;
            if (pty_idx >= 0) {
                tidx = 100 + pty_idx;
                pty_t *p = get_pty(pty_idx);
                if (p) p->fg_pgrp = current_task_ptr->pgid;
            } else {
                tidx = tty_rel_to_idx(rel_path);
                if (tidx >= 0) {
                    tty_t *t = get_tty(tidx);
                    if (t) {
                        t->fg_pgrp = current_task_ptr->pgid;
                        uint64_t irq;
                        spin_lock_irqsave(&tty_lock, &irq);
                        t->input.head = t->input.tail = 0;
                        spin_unlock_irqrestore(&tty_lock, irq);
                    }
                } else {
                    tidx = -1;
                }
            }
            if (tidx >= 0) {
                current_task_ptr->ctty_idx = tidx;
            }
        }

        frame->rax = (uint64_t)fd;
        return;
    }

    // procfs: /proc, /proc/self, /proc/<pid>, /proc/<pid>/{maps,mounts,exe,...}
    {
        int pr = proc_open_common(abs_path, sizeof(abs_path), flags);
        if (pr != 1) { frame->rax = (uint64_t)pr; return; }
    }

    // tmpfs (/tmp, /run, ...): RAM-backed, fully writable VFS
    {
        int tr = open_tmpfs_common(abs_path, flags, mode);
        if (tr != 1) { frame->rax = (uint64_t)tr; return; }
    }

    {
        int er = open_ext4_common(abs_path, flags);
        if (er != 1) { frame->rax = (uint64_t)er; return; }
    }

    initrd_file_t file = read_initrd(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    int parent_access = check_parent_access(abs_path, false);
    if (parent_access < 0) { frame->rax = (uint64_t)parent_access; return; }

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int access = check_parent_access(abs_path, true);
        if (access < 0) { frame->rax = (uint64_t)access; return; }
        int r = write_initrd(abs_path, "", 0, apply_current_umask(mode) | S_IFREG, current_task_ptr->fsuid, current_task_ptr->fsgid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
        file = read_initrd(abs_path);
    }

    if (!can_access_initrd(&file, want_read, want_write, 0)) { frame->rax = (uint64_t)-EACCES; return; }

    // O_TRUNC: truncate existing regular file to zero length
    if ((flags & O_TRUNC) && !want_write) { frame->rax = (uint64_t)-EACCES; return; }
    if ((flags & O_TRUNC) && file.data && S_ISREG(file.mode)) {
        int r = write_initrd(abs_path, NULL, 0, file.mode, file.uid, file.gid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
    }

    int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_FILE, flags);
    frame->rax = (uint64_t)fd;
}

void sys_close(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int res = free_fd(&current_task_ptr->fd_table, fd);
    frame->rax = (res < 0) ? (uint64_t)res : 0;
}

void sys_stat(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    struct stat *st = (struct stat *)frame->rsi;

    if (!user_path || !st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int cr = copy_string_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));
    strncpy(abs_path, resolved, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst) || stat_tmpfs_to_kst(abs_path, &kst, true) || stat_ext4_to_kst(abs_path, &kst, true)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (stat_proc(abs_path, path, &kst, true)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }

    if (!stat_initrd_to_kst(abs_path, &kst, true)) { frame->rax = (uint64_t)-ENOENT; return; }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_fstat(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    struct stat *st = (struct stat *)frame->rsi;

    if (!st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_DEV) {
        struct stat kst = {0};
        if (stat_virtual_device(entry->path, &kst)) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
    }

    // FD_STREAM fds (init's stdin/stdout/stderr) point at /dev/tty1: stat them
    // as that character device so fstat()/ttyname() behave correctly.
    if (entry->type == FD_STREAM) {
        struct stat kst = {0};
        if (stat_virtual_device("/dev/tty1", &kst)) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
    }

    if (entry->type == FD_FILE) {
        struct stat kst = {0};
        if (!stat_initrd_to_kst(entry->path, &kst, true)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (entry->type == FD_TMPFS) {
        struct stat kst = {0};
        if (stat_tmpfs_to_kst(entry->path, &kst, true)) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
    }
    if (entry->type == FD_EXT4) {
        struct stat kst = {0};
        int status = stat_ext4(entry->path, &kst, true);
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (entry->type == FD_PROC) {
        struct stat kst = {0};
        if (stat_proc(entry->path, NULL, &kst, true)) {
            if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
    }
    frame->rax = (uint64_t)-EBADF;
}

void sys_lstat(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    struct stat *st = (struct stat *)frame->rsi;

    if (!user_path || !st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int cr = copy_string_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    bool has_trailing_slash = false;
    size_t path_len = strlen(path);
    if (path_len > 0 && path[path_len - 1] == '/') {
        has_trailing_slash = true;
    }

    {
        char resolved[256];
        resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), has_trailing_slash);
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst) || stat_tmpfs_to_kst(abs_path, &kst, false) || stat_ext4_to_kst(abs_path, &kst, false)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (stat_proc(abs_path, path, &kst, has_trailing_slash)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }

    if (!stat_initrd_to_kst(abs_path, &kst, false)) { frame->rax = (uint64_t)-ENOENT; return; }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_poll(syscall_frame_t *frame) {
    struct pollfd *user_fds = (struct pollfd *)frame->rdi;
    uint64_t nfds = (uint64_t)frame->rsi;
    int timeout = (int)frame->rdx;
    if (nfds > 1024) { frame->rax = (uint64_t)-EINVAL; return; }
    if (nfds > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_fds, nfds * sizeof(struct pollfd))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    struct pollfd *k_fds = NULL;
    if (nfds > 0) {
        k_fds = malloc(nfds * sizeof(struct pollfd));
        if (!k_fds) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (copy_from_user(k_fds, user_fds, nfds * sizeof(struct pollfd)) < 0) {
            free(k_fds);
            frame->rax = (uint64_t)-EFAULT; return;
        }
    }

    #define EVAL_FDS(events) do { \
        (events) = 0; \
        for (uint64_t i = 0; i < nfds; i++) { \
            k_fds[i].revents = 0; \
            int fd = k_fds[i].fd; \
            if (fd < 0) continue; \
            fd_entry_t *entry = get_current_fd(fd); \
            if (!entry || !entry->open) { k_fds[i].revents |= POLLNVAL; (events)++; continue; } \
            if (k_fds[i].events & POLLIN) { \
                if (entry->type == FD_STREAM) { \
                    int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 1; \
                    tty_t *t = get_tty(tty_idx); \
                    if (t && get_tty_ring_count(&t->input) > 0) k_fds[i].revents |= POLLIN; \
                } else if (entry->type == FD_DEV) { \
                    char rel[256]; \
                    int tty_idx = -1; \
                    if (match_vfs_path(entry->path, "devtmpfs", rel)) { \
                        tty_idx = tty_rel_to_idx(rel); \
                    } else if (match_vfs_path(entry->path, "devpts", rel)) { \
                        tty_idx = current_task_ptr->ctty_idx; \
                    } \
                    if (tty_idx >= 0) { \
                        tty_t *t = get_tty(tty_idx); \
                        if (t && get_tty_ring_count(&t->input) > 0) k_fds[i].revents |= POLLIN; \
                    } else { \
                        k_fds[i].revents |= POLLIN; \
                    } \
                } else if (entry->type == FD_PIPE) { \
                    unix_handle_t *h = (unix_handle_t *)entry->handle; \
                    if (h && h->in && (h->in->len > 0 || h->in->writers == 0)) k_fds[i].revents |= POLLIN; \
                } else if (entry->type == FD_SOCKET) { \
                    poll_net_device(); \
                    if (is_socket_readable((socket_t *)entry->handle)) k_fds[i].revents |= POLLIN; \
                } else { \
                    k_fds[i].revents |= POLLIN; \
                } \
            } \
            if (k_fds[i].events & POLLOUT) k_fds[i].revents |= POLLOUT; \
            if (k_fds[i].revents) (events)++; \
        } \
    } while (0)

    int events = 0;
    EVAL_FDS(events);

    if (events == 0 && timeout != 0) {
        uint64_t start_time = get_monotonic_time_us();
        uint64_t total_us = (timeout > 0) ? (uint64_t)timeout * 1000ULL : UINT64_MAX;
        while (1) {
            if (signal_pending()) {
                if (nfds > 0) free(k_fds);
                frame->rax = (uint64_t)-EINTR;
                return;
            }
            EVAL_FDS(events);
            if (events > 0) break;
            if (timeout > 0 && get_monotonic_time_us() - start_time >= total_us) break;
            yield_sched();
        }
    }

    #undef EVAL_FDS

    if (nfds > 0) {
        if (copy_to_user((void*)user_fds, k_fds, nfds * sizeof(struct pollfd)) < 0) {
            frame->rax = (uint64_t)-EFAULT;
            free(k_fds);
            return;
        }
        free(k_fds);
    }
    frame->rax = (uint64_t)events;
}

void sys_lseek(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int64_t offset = (int64_t)frame->rsi;
    int whence = (int)frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry || !entry->open) { frame->rax = -EBADF; return; }
    if (entry->type == FD_STREAM || entry->type == FD_PIPE || entry->type == FD_SOCKET || entry->type == FD_PTY_MASTER) {
        frame->rax = -ESPIPE; 
        return; 
    }

    // Compute the file size for SEEK_END.  Proc files have no initrd backing.
    int64_t file_size = -1;
    bool block_device = false;
    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t size;
        if (match_vfs_path(entry->path, "devpts", rel) || !match_vfs_path(entry->path, "devtmpfs", rel)) {
            frame->rax = -ESPIPE;
            return;
        }
        int status = get_block_device_size(rel, &size);
        if (status < 0) { frame->rax = status == -ENOENT ? -ESPIPE : status; return; }
        if (size > INT64_MAX) { frame->rax = -EOVERFLOW; return; }
        file_size = (int64_t)size;
        block_device = true;
    } else if (entry->type == FD_PROC) {
        int self = proc_self_idx();
        proc_node_t n;
        if (resolve_procfs(entry->path, self, &n) && !is_procfs_dir(&n) && n.type != PROC_NODE_SYMLINK) {
            char tmp[PROCFS_MAX_CONTENT];
            file_size = (int64_t)get_procfs_content(&n, tmp);
        } else {
            file_size = 0;  // dirs and symlinks: SEEK_END lands at 0
        }
    } else if (entry->type == FD_TMPFS) {
        tmpfs_file_t file = read_tmpfs(entry->path);
        if (!file.mode) { frame->rax = -ENOENT; return; }
        file_size = (int64_t)file.size;
    } else if (entry->type == FD_EXT4) {
        struct stat st;
        int status = stat_ext4(entry->path, &st, true);
        if (status < 0) { frame->rax = status; return; }
        if (st.st_size < 0) { frame->rax = -EOVERFLOW; return; }
        file_size = st.st_size;
    } else {
        initrd_file_t file = read_initrd(entry->path);
        if (!file.mode) { frame->rax = -ENOENT; return; }
        file_size = (int64_t)file.size;
    }

    uint64_t new_offset;
    switch (whence) {
        case SEEK_SET:
            // Reject negative absolute positions
            if (offset < 0) { frame->rax = -EINVAL; return; }
            new_offset = (uint64_t)offset;
            break;
        case SEEK_CUR: {
            // Guard against signed overflow and underflow
            if (entry->offset > INT64_MAX) { frame->rax = -EOVERFLOW; return; }
            int64_t cur = (int64_t)entry->offset;
            if (offset > 0 && cur > INT64_MAX - offset) { frame->rax = -EOVERFLOW; return; }
            int64_t new_off = cur + offset;
            if (new_off < 0) { frame->rax = -EINVAL; return; }
            new_offset = (uint64_t)new_off;
            break;
        }
        case SEEK_END: {
            if (offset > 0 && file_size > INT64_MAX - offset) { frame->rax = -EOVERFLOW; return; }
            int64_t new_off = file_size + offset;
            if (new_off < 0) { frame->rax = -EINVAL; return; }
            new_offset = (uint64_t)new_off;
            break;
        }
        default:
            frame->rax = -EINVAL;
            return;
    }

    if (block_device && new_offset > (uint64_t)file_size) { frame->rax = -EINVAL; return; }
    entry->offset = new_offset;
    frame->rax = new_offset;
}

static void rollback_mmap(void *ptr, uint64_t num_pages, uint64_t retained_pages) {
    if (!ptr) return;
    uint64_t start = (uint64_t)ptr;
    for (uint64_t i = 0; i < num_pages; i++) unmap_vmm(current_task_ptr->ctx, start + i * PAGE_SIZE);
    remove_vma(&current_task_ptr->ctx->vmas, start, start + num_pages * PAGE_SIZE);
    current_task_ptr->ctx->mmap_pages = retained_pages;
}

void sys_mmap(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;
    int      flags  = (int)frame->r10;
    int      fd     = (int)frame->r8;
    uint64_t offset = frame->r9;

    if (length == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (offset & (PAGE_SIZE - 1)) { frame->rax = (uint64_t)-EINVAL; return; }

    // Guard against integer overflow in page-count calculation
    if (length > USER_ADDR_MAX) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t ignored_start;
    uint64_t ignored_end;
    // Validate addr if MAP_FIXED or hint provided, including page rounding.
    if (addr != 0 && !user_page_range_ok(addr, length, &ignored_start, &ignored_end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    bool fixed = (flags & MAP_FIXED) != 0;
    bool fixed_noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;
    if ((fixed || fixed_noreplace) && (addr & (PAGE_SIZE - 1))) { frame->rax = (uint64_t)-EINVAL; return; }

    // Reject W+X mappings (W^X policy)
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        frame->rax = (uint64_t)-EACCES; return;
    }

    uint64_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    // Overflow check: ensure num_pages * PAGE_SIZE doesn't wrap
    if (num_pages > (USER_ADDR_MAX / PAGE_SIZE)) { frame->rax = (uint64_t)-EINVAL; return; }
    uint64_t map_size = num_pages * PAGE_SIZE;
    if (addr != 0 && !user_page_range_ok(addr, map_size, &ignored_start, &ignored_end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    bool anonymous = (flags & MAP_ANONYMOUS) != 0;
    if (!anonymous && offset > UINT64_MAX - map_size) { frame->rax = (uint64_t)-EINVAL; return; }
    bool requested_fb = false;
    struct limine_framebuffer *mapped_fb = NULL;
    fd_entry_t *mapping_entry = NULL;
    if (!anonymous) {
        mapping_entry = get_current_fd(fd);
        if (!mapping_entry) { frame->rax = (uint64_t)-EBADF; return; }
        if (!fd_allows_read(mapping_entry)) { frame->rax = (uint64_t)-EACCES; return; }
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !fd_allows_write(mapping_entry)) {
            frame->rax = (uint64_t)-EACCES;
            return;
        }
        char rel[256];
        if (mapping_entry->type == FD_DEV && match_vfs_path(mapping_entry->path, "devtmpfs", rel) && rel[0] == 'f' && rel[1] == 'b' && rel[2] >= '0' && rel[2] <= '9' && rel[3] == '\0') {
            requested_fb = true;
            if (current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EACCES; return; }
            int idx = rel[2] - '0';
            if (!fb_req.response || idx >= (int)fb_req.response->framebuffer_count) { frame->rax = (uint64_t)-ENODEV; return; }
            mapped_fb = fb_req.response->framebuffers[idx];
            if (!mapped_fb || (mapped_fb->height && mapped_fb->pitch > UINT64_MAX / mapped_fb->height)) { frame->rax = (uint64_t)-EINVAL; return; }
            uint64_t fb_size = mapped_fb->height * mapped_fb->pitch;
            uint64_t available_pages = offset < fb_size ? (fb_size - offset + PAGE_SIZE - 1) / PAGE_SIZE : 0;
            if (num_pages > available_pages) { frame->rax = (uint64_t)-EINVAL; return; }
        }
        if (!requested_fb && mapping_entry->type != FD_FILE && mapping_entry->type != FD_TMPFS && mapping_entry->type != FD_EXT4) {
            frame->rax = (uint64_t)-ENODEV;
            return;
        }
    }

    uint64_t replaced_pages = 0;
    if (fixed) {
        replaced_pages = flagged_vma_pages_in_range(&current_task_ptr->ctx->vmas, addr, addr + num_pages * PAGE_SIZE, VMA_FLAG_MMAP);
        if (replaced_pages > current_task_ptr->ctx->mmap_pages) replaced_pages = current_task_ptr->ctx->mmap_pages;
    }
    uint64_t retained_pages = current_task_ptr->ctx->mmap_pages - replaced_pages;
    if (num_pages > MAX_USER_MMAP_PAGES - retained_pages) {
        frame->rax = (uint64_t)-ENOMEM;
        return;
    }

    uint64_t vmm_flags = VMM_USER;
    if (prot & PROT_WRITE) vmm_flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_NX;
    if (flags & MAP_SHARED) vmm_flags |= VMM_SHARED;

    void *ptr = NULL;
    if (fixed_noreplace && user_range_is_mapped(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE)) {
        frame->rax = (uint64_t)-EEXIST;
        return;
    }
    if (fixed || fixed_noreplace) {
        // MAP_FIXED must *replace* any existing mapping in [addr, addr+size):
        // POSIX/Linux semantics are that the old mappings are discarded, not
        // merged.  ld.so relies on this when it overlays an anonymous
        // (zero-initialised) BSS-overflow map on top of an earlier file-backed
        // mapping of the same address range.  Without this unmap step, the
        // "already mapped, just update flags" branch in vmap_user_at silently
        // keeps the old *physical* page and its stale file bytes — so BSS lock
        // words end up holding garbage from the previously-mapped file
        // (observed: libc.so.6 BSS at runtime contained "inet..." ASCII bytes
        // from the file, breaking __lll_lock_wait_private which then sees
        // val != 0, writes 2, and deadlocks).
        uint64_t fixed_start = addr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t fixed_end   = (addr + num_pages * PAGE_SIZE + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        for (uint64_t a = fixed_start; a < fixed_end; a += PAGE_SIZE) {
            if (get_vmm_pte(current_task_ptr->ctx, a) & (VMM_PRESENT | VMM_DEMAND)) unmap_vmm(current_task_ptr->ctx, a);
        }
        current_task_ptr->ctx->mmap_pages = retained_pages;
        ptr = vmap_user_at(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE, vmm_flags);
    } else if ((flags & MAP_32BIT) && addr == 0) {
        ptr = vmap_user_range_32(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    } else if (addr != 0 && !user_range_is_mapped(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE)) {
        ptr = vmap_user_at(current_task_ptr->ctx, addr & ~(PAGE_SIZE - 1), num_pages * PAGE_SIZE, vmm_flags);
        if (!ptr) ptr = vmap_user_range(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    } else {
        ptr = vmap_user_range(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    }

    if (!ptr) {
        frame->rax = (uint64_t)-ENOMEM; return;
    }
    current_task_ptr->ctx->mmap_pages = retained_pages + num_pages;

    bool fb_mapped = false;
    if (requested_fb && mapped_fb) {
        uint64_t phys_base = virt_to_phys((void *)mapped_fb->address);
        if ((phys_base & (PAGE_SIZE - 1)) || phys_base > UINT64_MAX - offset || phys_base + offset > UINT64_MAX - map_size) {
            rollback_mmap(ptr, num_pages, retained_pages);
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
        uint64_t map_flags = VMM_USER | VMM_PWT | VMM_PCD | VMM_NX | VMM_EXTERNAL;
        if (prot & PROT_WRITE) map_flags |= VMM_WRITABLE;

        for (uint64_t i = 0; i < num_pages; i++) {
            uint64_t vaddr = (uint64_t)ptr + i * PAGE_SIZE;
            if (get_vmm_phys(current_task_ptr->ctx, vaddr) != 0) unmap_vmm(current_task_ptr->ctx, vaddr);
            if (!map_vmm(current_task_ptr->ctx, vaddr, phys_base + offset + i * PAGE_SIZE, map_flags)) {
                rollback_mmap(ptr, num_pages, retained_pages);
                frame->rax = (uint64_t)-ENOMEM;
                return;
            }
        }
        fb_mapped = true;
    }

    // Record the VMA so /proc/<pid>/maps can describe this mapping.
    {
        int vprot = 0;
        if (prot & PROT_READ)  vprot |= VMA_PROT_READ;
        if (prot & PROT_WRITE) vprot |= VMA_PROT_WRITE;
        if (prot & PROT_EXEC)  vprot |= VMA_PROT_EXEC;
        int vflags = 0;
        if (flags & MAP_ANONYMOUS) vflags |= VMA_FLAG_ANON;
        if (flags & MAP_SHARED)    vflags |= VMA_FLAG_SHARED;
        const char *name = NULL;
        char namebuf[256];
        if (!anonymous) {
            strncpy(namebuf, mapping_entry->path, sizeof(namebuf) - 1);
            namebuf[sizeof(namebuf) - 1] = '\0';
            name = namebuf;
        }
        if (!add_vma(&current_task_ptr->ctx->vmas, (uint64_t)ptr, (uint64_t)ptr + num_pages * PAGE_SIZE, vprot, vflags | VMA_FLAG_MMAP, offset, name)) {
            rollback_mmap(ptr, num_pages, retained_pages);
            frame->rax = (uint64_t)-ENOMEM;
            return;
        }
    }

    // File-backed mapping: copy data if not anonymous
    if (!anonymous) {
        if (fb_mapped) { frame->rax = (uint64_t)ptr; return; }
        fd_entry_t *entry = mapping_entry;

        if (entry->type == FD_EXT4) {
            uint8_t *chunk = malloc(65536);
            if (!chunk) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-ENOMEM; return; }
            uint64_t copied = 0;
            uint64_t map_size = num_pages * PAGE_SIZE;
            while (copied < map_size) {
                uint64_t amount = map_size - copied;
                if (amount > 65536) amount = 65536;
                int64_t got = read_ext4(entry->path, chunk, amount, offset + copied);
                if (got < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)got; return; }
                if (got == 0) break;
                if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr + copied, chunk, (uint64_t)got) < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; }
                copied += (uint64_t)got;
                if ((uint64_t)got < amount) break;
            }
            free(chunk);
        } else if (entry->type == FD_TMPFS) {
            tmpfs_file_t file = read_tmpfs(entry->path);
            uint64_t map_size = num_pages * PAGE_SIZE;
            uint64_t file_avail = file.size > offset ? file.size - offset : 0;
            uint64_t copy_len = file_avail < map_size ? file_avail : map_size;
            if (copy_len && file.data) { if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr, (uint8_t *)file.data + offset, copy_len) < 0) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; } }
        } else {
            initrd_file_t file = read_initrd(entry->path);
            if (file.data) {
            // Validate offset is within file bounds to prevent out-of-bounds read
            if (offset >= file.size && file.size > 0) {
                // Nothing to copy, mapping is zero-filled
            } else {
                uint64_t map_size = num_pages * PAGE_SIZE;
                uint64_t file_avail = (file.size > offset) ? (file.size - offset) : 0;
                uint64_t copy_len = (file_avail < map_size) ? file_avail : map_size;
                if (copy_len > 0) { if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr, (uint8_t *)file.data + offset, copy_len) < 0) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; } }
            }
            // Zero the remaining bytes (BSS-like) is already handled by vmap_user_at/vmalloc_ex
            // which zeroed the newly allocated pages.
            }
        }
    }

    frame->rax = (uint64_t)ptr;
}

void sys_mprotect(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;

    if (length == 0) { frame->rax = 0; return; }
    // Ensure entire range is in user-space
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        frame->rax = (uint64_t)-EACCES; return;
    }

    uint64_t start;
    uint64_t end;
    if (!user_page_range_ok(addr, length, &start, &end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t phys = get_vmm_phys(current_task_ptr->ctx, a);
        if (!phys) { frame->rax = (uint64_t)-ENOMEM; return; }
        uint64_t vmm_flags = VMM_USER;
        if (prot & PROT_WRITE) vmm_flags |= VMM_WRITABLE;
        if (!(prot & PROT_EXEC)) vmm_flags |= VMM_NX;
        map_vmm(current_task_ptr->ctx, a, phys, vmm_flags);
    }

    // Reflect the protection change in the VMA table.
    {
        int vprot = 0;
        if (prot & PROT_READ)  vprot |= VMA_PROT_READ;
        if (prot & PROT_WRITE) vprot |= VMA_PROT_WRITE;
        if (prot & PROT_EXEC)  vprot |= VMA_PROT_EXEC;
        protect_vma(&current_task_ptr->ctx->vmas, start, end, vprot);
    }

    frame->rax = 0;
}

void sys_munmap(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;

    if (length == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    // Ensure entire range is in user-space
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t start;
    uint64_t end;
    if (!user_page_range_ok(addr, length, &start, &end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    uint64_t removed_pages = flagged_vma_pages_in_range(&current_task_ptr->ctx->vmas, start, end, VMA_FLAG_MMAP);
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        unmap_vmm(current_task_ptr->ctx, a);
    }

    if (removed_pages > current_task_ptr->ctx->mmap_pages) removed_pages = current_task_ptr->ctx->mmap_pages;
    current_task_ptr->ctx->mmap_pages -= removed_pages;

    // Drop the now-unmapped range from the VMA table.
    remove_vma(&current_task_ptr->ctx->vmas, start, end);

    frame->rax = 0;
}

void sys_brk(syscall_frame_t *frame) {
    uint64_t addr = frame->rdi;

    if (addr == 0) {
        // Return current break
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Validate: new brk must be in user-space and above brk_start
    if (!user_address_range_ok(addr, 1)) { frame->rax = current_task_ptr->brk; return; }
    if (addr < current_task_ptr->brk_start) {
        // Cannot shrink below initial heap start
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Enforce upper bound to prevent unconstrained heap growth
    if (addr - current_task_ptr->brk_start > MAX_BRK_SIZE) {
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Align to page boundary
    uint64_t old_brk = current_task_ptr->brk & ~0xFFFULL;
    if (addr > UINT64_MAX - (PAGE_SIZE - 1)) {
        frame->rax = current_task_ptr->brk;
        return;
    }
    uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_brk > old_brk) {
        // Map new pages
        for (uint64_t a = old_brk; a < new_brk; a += 4096) {
            if (get_vmm_phys(current_task_ptr->ctx, a) == 0) {
                void *page = pmalloc();
                if (!page) {
                    for (uint64_t rollback = old_brk; rollback < a; rollback += PAGE_SIZE) unmap_vmm(current_task_ptr->ctx, rollback);
                    frame->rax = current_task_ptr->brk;
                    return;
                }
                if (!map_vmm(current_task_ptr->ctx, a, (uint64_t)page, VMM_USER | VMM_WRITABLE | VMM_NX)) {
                    pfree(page);
                    for (uint64_t rollback = old_brk; rollback < a; rollback += PAGE_SIZE) unmap_vmm(current_task_ptr->ctx, rollback);
                    frame->rax = current_task_ptr->brk;
                    return;
                }
                (void)memset_vmm(current_task_ptr->ctx, a, 0, 4096);
            }
        }
    }

    current_task_ptr->brk = addr;
    // Keep the [heap] VMA in sync with the break so /proc/<pid>/maps is accurate.
    set_vma_heap(&current_task_ptr->ctx->vmas, current_task_ptr->brk_start, current_task_ptr->brk);
    frame->rax = current_task_ptr->brk;
}

void sys_rt_sigaction(syscall_frame_t *frame) {
    int signum = (int)frame->rdi;
    uint64_t act_ptr = frame->rsi;
    uint64_t oldact_ptr = frame->rdx;

    if (signum < 1 || signum > 31 || (act_ptr && (signum == SIGKILL || signum == SIGSTOP))) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    if (oldact_ptr) {
        if (copy_to_user((void *)oldact_ptr, &current_task_ptr->sigactions[signum * 4], 32) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }
    if (act_ptr) {
        uint64_t new_sa[4];
        if (copy_from_user(new_sa, (const void *)act_ptr, 32) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (new_sa[0] > (uint64_t)SIG_IGN && !user_address_range_ok(new_sa[0], 1)) { frame->rax = (uint64_t)-EINVAL; return; }
        if (new_sa[2] && !user_address_range_ok(new_sa[2], 1)) { frame->rax = (uint64_t)-EINVAL; return; }
        for (int k = 0; k < 4; k++) current_task_ptr->sigactions[signum * 4 + k] = new_sa[k];
    }
    frame->rax = 0;
}

void sys_rt_sigprocmask(syscall_frame_t *frame) {
    int how = (int)frame->rdi;
    const uint64_t *set = (const uint64_t *)frame->rsi;
    uint64_t *oldset = (uint64_t *)frame->rdx;
    size_t sigsetsize = (size_t)frame->r10;

    if (sigsetsize != 8) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    if (oldset) {
        if (copy_to_user(oldset, &current_task_ptr->blocked_signals, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    if (set) {
        uint64_t new_set;
        if (copy_from_user(&new_set, set, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

        new_set &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

        if (how == SIG_BLOCK) {
            current_task_ptr->blocked_signals |= new_set;
        } else if (how == SIG_UNBLOCK) {
            current_task_ptr->blocked_signals &= ~new_set;
        } else if (how == SIG_SETMASK) {
            current_task_ptr->blocked_signals = new_set;
        } else {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
    }

    frame->rax = 0;
}

void sys_rt_sigreturn(syscall_frame_t *frame) {
    uint64_t user_rsp = frame->rsp;
    signal_stack_frame_t saved;
    if (user_rsp >= USER_ADDR_MAX - 16 || copy_from_user(&saved, (const void *)(user_rsp + 16), sizeof(saved)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    syscall_frame_t saved_frame = saved.context;
    if (saved_frame.rip >= USER_ADDR_MAX || saved_frame.rsp >= USER_ADDR_MAX) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    saved_frame.rflags &= ~USER_RFLAGS_FORBIDDEN;
    saved_frame.rflags |= RFLAGS_FIXED;
    current_task_ptr->blocked_signals = saved.blocked_signals & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    *frame = saved_frame;
}

void sys_ioctl(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    unsigned long req = (unsigned long)frame->rsi;
    uint64_t argp = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);

    if (entry && entry->type == FD_SOCKET) {
        static short interface_flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
        static int interface_mtu = 1500;
        static int interface_tx_queue_length = 1000;

        bool network_admin_request = req == SIOCADDRT || req == SIOCDELRT || req == SIOCSIFFLAGS || req == SIOCSIFADDR || req == SIOCSIFNETMASK || req == SIOCSIFBRDADDR || req == SIOCSIFMTU || req == SIOCSIFTXQLEN;
        if (network_admin_request && (!current_task_ptr || current_task_ptr->euid != 0)) {
            frame->rax = (uint64_t)-EPERM;
            return;
        }

        if (req == SIOCADDRT || req == SIOCDELRT) {
            struct rtentry route;
            if (copy_from_user(&route, (const void *)argp, sizeof(route)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (req == SIOCADDRT && (route.rt_flags & RTF_GATEWAY)) memcpy(&net_gateway_ip, route.rt_gateway.sa_data + 2, sizeof(net_gateway_ip));
            if (req == SIOCDELRT) net_gateway_ip = 0;
            frame->rax = 0;
            return;
        }

        bool interface_query = req == SIOCGIFNAME || req == SIOCGIFINDEX || req == SIOCGIFHWADDR || req == SIOCGIFFLAGS || req == SIOCSIFFLAGS || req == SIOCGIFADDR || req == SIOCSIFADDR || req == SIOCGIFNETMASK || req == SIOCSIFNETMASK || req == SIOCGIFBRDADDR || req == SIOCSIFBRDADDR || req == SIOCGIFMTU || req == SIOCSIFMTU || req == SIOCGIFTXQLEN || req == SIOCSIFTXQLEN;
        if (interface_query) {
            struct ifreq ifr;
            if (copy_from_user(&ifr, (const void *)argp, sizeof(ifr)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';
            if (req == SIOCGIFNAME) {
                if (ifr.ifr_ifindex == NET_LOOPBACK_INTERFACE_INDEX) strncpy(ifr.ifr_name, "lo", IFNAMSIZ);
                else if (ifr.ifr_ifindex == NET_ETHERNET_INTERFACE_INDEX && net_current_device) strncpy(ifr.ifr_name, "eth0", IFNAMSIZ);
                else { frame->rax = (uint64_t)-ENODEV; return; }
            } else if (strcmp(ifr.ifr_name, "lo") != 0 && (strcmp(ifr.ifr_name, "eth0") != 0 || !net_current_device)) {
                frame->rax = (uint64_t)-ENODEV;
                return;
            } else if (req == SIOCGIFINDEX) {
                ifr.ifr_ifindex = strcmp(ifr.ifr_name, "lo") == 0 ? NET_LOOPBACK_INTERFACE_INDEX : NET_ETHERNET_INTERFACE_INDEX;
            } else if (req == SIOCGIFHWADDR) {
                if (!net_current_device) { frame->rax = (uint64_t)-ENODEV; return; }
                ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
                memcpy(ifr.ifr_hwaddr.sa_data, net_current_device->mac, 6);
            } else if (req == SIOCGIFFLAGS) {
                ifr.ifr_flags = interface_flags;
            } else if (req == SIOCSIFFLAGS) {
                interface_flags = ifr.ifr_flags;
            } else if (req == SIOCGIFADDR) {
                ifr.ifr_addr.sa_family = AF_INET;
                memcpy(ifr.ifr_addr.sa_data + 2, &net_local_ip, sizeof(net_local_ip));
            } else if (req == SIOCSIFADDR) {
                memcpy(&net_local_ip, ifr.ifr_addr.sa_data + 2, sizeof(net_local_ip));
            } else if (req == SIOCGIFNETMASK) {
                ifr.ifr_netmask.sa_family = AF_INET;
                memcpy(ifr.ifr_netmask.sa_data + 2, &net_subnet_mask, sizeof(net_subnet_mask));
            } else if (req == SIOCSIFNETMASK) {
                memcpy(&net_subnet_mask, ifr.ifr_netmask.sa_data + 2, sizeof(net_subnet_mask));
            } else if (req == SIOCGIFBRDADDR) {
                uint32_t broadcast = net_local_ip | ~net_subnet_mask;
                ifr.ifr_broadaddr.sa_family = AF_INET;
                memcpy(ifr.ifr_broadaddr.sa_data + 2, &broadcast, sizeof(broadcast));
            } else if (req == SIOCSIFBRDADDR) {
            } else if (req == SIOCGIFMTU) {
                ifr.ifr_mtu = interface_mtu;
            } else if (req == SIOCSIFMTU) {
                if (ifr.ifr_mtu < 68 || ifr.ifr_mtu > 1500) { frame->rax = (uint64_t)-EINVAL; return; }
                interface_mtu = ifr.ifr_mtu;
            } else if (req == SIOCGIFTXQLEN) {
                ifr.ifr_qlen = interface_tx_queue_length;
            } else if (req == SIOCSIFTXQLEN) {
                if (ifr.ifr_qlen < 0) { frame->rax = (uint64_t)-EINVAL; return; }
                interface_tx_queue_length = ifr.ifr_qlen;
            } else {
                frame->rax = (uint64_t)-EINVAL;
                return;
            }
            if (copy_to_user((void *)argp, &ifr, sizeof(ifr)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
    }

    // Handle framebuffer ioctl requests
    if (entry && entry->type == FD_DEV) {
        char rel[256];
        if (match_vfs_path(entry->path, "devtmpfs", rel)) {
            if (strncmp(rel, "fb", 2) == 0) {
                int idx = rel[2] - '0';
                if (fb_req.response && idx >= 0 && idx < (int)fb_req.response->framebuffer_count) {
                    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
                    if (req == FBIOGET_VSCREENINFO) {
                        struct fb_var_screeninfo vinfo;
                        memset(&vinfo, 0, sizeof(vinfo));
                        vinfo.xres = fb->width;
                        vinfo.yres = fb->height;
                        vinfo.xres_virtual = idx == 0 && fb_xres_virtual ? fb_xres_virtual : fb->width;
                        vinfo.yres_virtual = idx == 0 && fb_yres_virtual ? fb_yres_virtual : fb->height;
                        vinfo.xoffset = idx == 0 ? fb_xoffset : 0;
                        vinfo.yoffset = idx == 0 ? fb_yoffset : 0;
                        vinfo.bits_per_pixel = fb->bpp;
                        // Set RGB bitfield layout.  Prefer limine's mask sizes
                        // but fall back to safe defaults when they are zero
                        // (e.g. some firmware framebuffers don't fill them in).
                        if (fb->bpp == 32) {
                            if (fb->red_mask_size) {
                                vinfo.red.offset   = fb->red_mask_shift;
                                vinfo.red.length   = fb->red_mask_size;
                                vinfo.green.offset = fb->green_mask_shift;
                                vinfo.green.length = fb->green_mask_size;
                                vinfo.blue.offset  = fb->blue_mask_shift;
                                vinfo.blue.length  = fb->blue_mask_size;
                            } else {
                                // BGRA8 by default (QEMU/Bochs layout)
                                vinfo.blue.offset  = 0;  vinfo.blue.length  = 8;
                                vinfo.green.offset = 8;  vinfo.green.length = 8;
                                vinfo.red.offset   = 16; vinfo.red.length   = 8;
                            }
                        } else if (fb->bpp == 24) {
                            vinfo.red.offset   = 16; vinfo.red.length   = 8;
                            vinfo.green.offset = 8;  vinfo.green.length = 8;
                            vinfo.blue.offset  = 0;  vinfo.blue.length  = 8;
                        } else if (fb->bpp == 16) {
                            vinfo.red.offset   = 11; vinfo.red.length   = 5;
                            vinfo.green.offset = 5;  vinfo.green.length = 6;
                            vinfo.blue.offset  = 0;  vinfo.blue.length  = 5;
                        }
                        vinfo.activate = 0; // FB_ACTIVATE_NOW
                        if (copy_to_user((void *)argp, &vinfo, sizeof(vinfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOGET_FSCREENINFO) {
                        struct fb_fix_screeninfo finfo;
                        memset(&finfo, 0, sizeof(finfo));
                        strncpy(finfo.id, "limine-fb", 15);
                        finfo.smem_start = virt_to_phys((void *)fb->address);
                        finfo.smem_len = (idx == 0 && fb_yres_virtual ? fb_yres_virtual : fb->height) * fb->pitch;
                        finfo.type = FB_TYPE_PACKED_PIXELS;
                        finfo.visual = FB_VISUAL_TRUECOLOR;
                        finfo.line_length = fb->pitch;
                        if (copy_to_user((void *)argp, &finfo, sizeof(finfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOPUT_VSCREENINFO) {
                        if (idx != 0) { frame->rax = (uint64_t)-ENOTSUP; return; }
                        struct fb_var_screeninfo vinfo;
                        if (copy_from_user(&vinfo, (const void *)argp, sizeof(vinfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        uint64_t xres_virtual = vinfo.xres_virtual ? vinfo.xres_virtual : vinfo.xres;
                        uint64_t yres_virtual = vinfo.yres_virtual ? vinfo.yres_virtual : vinfo.yres;
                        int status = set_fb_resolution(vinfo.xres, vinfo.yres, xres_virtual, yres_virtual, vinfo.xoffset, vinfo.yoffset, (uint16_t)vinfo.bits_per_pixel);
                        if (status < 0) { frame->rax = (uint64_t)status; return; }
                        vinfo.xres = fb->width;
                        vinfo.yres = fb->height;
                        vinfo.xres_virtual = fb_xres_virtual;
                        vinfo.yres_virtual = fb_yres_virtual;
                        vinfo.xoffset = fb_xoffset;
                        vinfo.yoffset = fb_yoffset;
                        vinfo.bits_per_pixel = fb->bpp;
                        vinfo.red.offset = fb->red_mask_shift;
                        vinfo.red.length = fb->red_mask_size;
                        vinfo.green.offset = fb->green_mask_shift;
                        vinfo.green.length = fb->green_mask_size;
                        vinfo.blue.offset = fb->blue_mask_shift;
                        vinfo.blue.length = fb->blue_mask_size;
                        if (copy_to_user((void *)argp, &vinfo, sizeof(vinfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOPAN_DISPLAY) {
                        // Pan/offset — accept as no-op (no virtual screen pan).
                        frame->rax = 0;
                        return;
                    }
                }
            }
        }
    }

    int is_tty = (fd == 0 || fd == 1 || fd == 2);

    // Also treat devtmpfs tty devices as ttys
    if (!is_tty) {
        if (entry && entry->type == FD_DEV) {
            char rel[256];
            if (match_vfs_path(entry->path, "devtmpfs", rel)) {
                if (strncmp(rel, "tty", 3) == 0 || strncmp(rel, "pts/", 4) == 0 || strcmp(rel, "console") == 0) is_tty = 1;
            } else if (match_vfs_path(entry->path, "devpts", rel)) {
                is_tty = 1;
            }
        }
    }

    switch (req) {
        case KDGKBENT: {
            struct kbentry entry_map;
            if (!is_tty) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (copy_from_user(&entry_map, (const void *)argp, sizeof(entry_map)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            entry_map.kb_value = get_tty_keymap(entry_map.kb_table, entry_map.kb_index);
            if (copy_to_user((void *)argp, &entry_map, sizeof(entry_map)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case KDSKBENT: {
            struct kbentry entry_map;
            if (!is_tty) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
            if (copy_from_user(&entry_map, (const void *)argp, sizeof(entry_map)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (set_tty_keymap(entry_map.kb_table, entry_map.kb_index, entry_map.kb_value) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            frame->rax = 0;
            return;
        }

        case KDFONTOP: {
            int idx = ioctl_tty_idx(entry);
            if (!is_tty || idx < 0 || idx >= 100) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }

            struct console_font_op font_op;
            if (copy_from_user(&font_op, (const void *)argp, sizeof(font_op)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (font_op.op != KD_FONT_OP_SET || (font_op.flags & ~KD_FONT_FLAG_DONT_RECALC) != 0 || font_op.width != 8 || font_op.height == 0 || font_op.height > 32 || font_op.charcount != 256 || !font_op.data) { frame->rax = (uint64_t)-EINVAL; return; }

            uint64_t font_size = 256ULL * font_op.height;
            unsigned char *font_data = malloc(font_size);
            if (!font_data) { frame->rax = (uint64_t)-ENOMEM; return; }

            for (uint64_t glyph = 0; glyph < 256; glyph++) {
                uint64_t user_glyph = (uint64_t)font_op.data + glyph * 32ULL;
                if (copy_from_user(font_data + glyph * font_op.height, (const void *)user_glyph, font_op.height) < 0) { free(font_data); frame->rax = (uint64_t)-EFAULT; return; }
            }

            int status = change_font_data(font_data, 8, (uint8_t)font_op.height);
            free(font_data);
            frame->rax = (uint64_t)status;
            return;
        }

        case TCGETS: {
            struct termios t = {0};
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) {
                frame->rax = (uint64_t)-ENOTTY; return;
            }
            if (idx >= 100) {
                pty_t *pty_tcgets = get_pty(idx - 100);
                if (!pty_tcgets || !pty_tcgets->allocated) {
                    frame->rax = (uint64_t)-ENOTTY; return;
                }
                t = pty_tcgets->termios;
            } else {
                tty_t *tty_tcgets = get_tty(idx);
                if (!tty_tcgets) {
                    frame->rax = (uint64_t)-ENOTTY; return;
                }
                t = tty_tcgets->termios;
            }
        
            if (copy_to_user((void *)argp, &t, sizeof(t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (idx >= 100) {
                pty_t *pty_tcsets = get_pty(idx - 100);
                if (!pty_tcsets || !pty_tcsets->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                bool pty_was_icanon = !!(pty_tcsets->termios.c_lflag & ICANON);
                if (copy_from_user(&pty_tcsets->termios, (const void *)argp, sizeof(struct termios)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                bool pty_now_icanon = !!(pty_tcsets->termios.c_lflag & ICANON);
                if ((req == TCSETSF) || (pty_was_icanon && !pty_now_icanon)) {
                    pty_tcsets->m2s.head = pty_tcsets->m2s.tail = 0;
                }
            } else {
                tty_t *tty_tcsets = get_tty(idx);
                if (!tty_tcsets) { frame->rax = (uint64_t)-ENOTTY; return; }
                // Same reasoning as the PTY branch above.
                bool tty_was_icanon = !!(tty_tcsets->termios.c_lflag & ICANON);
                if (copy_from_user(&tty_tcsets->termios, (const void *)argp, sizeof(struct termios)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                bool tty_now_icanon = !!(tty_tcsets->termios.c_lflag & ICANON);
                if ((req == TCSETSF) || (tty_was_icanon && !tty_now_icanon)) {
                    uint64_t irq;
                    spin_lock_irqsave(&tty_lock, &irq);
                    tty_tcsets->input.head = tty_tcsets->input.tail = 0;
                    spin_unlock_irqrestore(&tty_lock, irq);
                    // Also drop any half-cooked canonical line in this task.
                    spin_lock_irqsave(&stdin_lock, &irq);
                    current_task_ptr->stdin_buf_len  = 0;
                    current_task_ptr->stdin_buf_pos  = 0;
                    spin_unlock_irqrestore(&stdin_lock, irq);
                }
            }
            frame->rax = 0;
            return;
        }

        case TIOCGWINSZ: {
            // Calculate real terminal size from framebuffer + font metrics
            winsize_t ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };
            if (fb_req.response && fb_req.response->framebuffer_count > 0) {
                struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
                ws.ws_xpixel = (uint16_t)fb->width;
                ws.ws_ypixel = (uint16_t)fb->height;
                if (current_font_w > 0 && current_font_h > 0) {
                    ws.ws_col = (uint16_t)(fb->width / current_font_w);
                    ws.ws_row = (uint16_t)(fb->height / current_font_h);
                }
            }
            if (copy_to_user((void *)argp, &ws, sizeof(ws)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TIOCSWINSZ:
            // Accept but ignore window size sets
            frame->rax = 0;
            return;

        case TCSBRK:
        case TCXONC:
            frame->rax = 0;
            return;

        case TCFLSH:
            if (entry && entry->type == FD_DEV) {
                char rel[256];
                if (match_vfs_path(entry->path, "devtmpfs", rel)) {
                    int idx = tty_rel_to_idx(rel);
                    if (idx >= 0 && idx < NUM_TTYS) {
                        if (argp == 0 || argp == 2) {
                            get_tty(idx)->input.head = get_tty(idx)->input.tail = 0;
                        }
                    }
                } else if (match_vfs_path(entry->path, "devpts", rel)) {
                    int idx = 0;
                    const char *p = rel;
                    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
                        if (idx >= 0 && idx < NUM_PTYS) {
                            if (argp == 0 || argp == 2) {
                            }
                        }
                    }
                }
            frame->rax = 0;
            return;

        case TIOCGPGRP: {
            // Return the foreground process group of the controlling tty.
            // Resolve the idx the same way sys_read() does so the value the
            // shell reads here matches the terminal read() actually gates.
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) {
                frame->rax = (uint64_t)-ENOTTY; return;
            }
            pid_t pgrp = current_task_ptr->pgid;
            if (idx >= 100) {
                pty_t *p = get_pty(idx - 100);
                if (p && p->fg_pgrp > 0) pgrp = p->fg_pgrp;
            } else {
                tty_t *tty_fg = get_tty(idx);
                if (tty_fg && tty_fg->fg_pgrp > 0) pgrp = tty_fg->fg_pgrp;
            }
            if(argp){
                pid_t pgrp_val = pgrp;
                if (copy_to_user((void *)argp, &pgrp_val, sizeof(pid_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            }
            frame->rax = 0;
            return;
        }

        case TIOCSPGRP: {
            // Set the foreground process group of the controlling tty.
            pid_t new_pgrp = 0;
            if (copy_from_user(&new_pgrp, (const void *)argp, sizeof(pid_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }

            if (idx >= 100) {
                pty_t *p = get_pty(idx - 100);
                if (!p) { frame->rax = (uint64_t)-ENOTTY; return; }
                p->fg_pgrp = new_pgrp;
            } else {
                tty_t *tty_sp = get_tty(idx);
                if (!tty_sp) { frame->rax = (uint64_t)-ENOTTY; return; }
                tty_sp->fg_pgrp = new_pgrp;
            }
            frame->rax = 0;
            return;
        }

        case FIONREAD: {
            // Report bytes available in per-task stdin buffer; 0 for everything else
            uint64_t irq;
            spin_lock_irqsave(&stdin_lock, &irq);
            int avail = (fd == 0) ? (current_task_ptr->stdin_buf_len - current_task_ptr->stdin_buf_pos) : 0;
            spin_unlock_irqrestore(&stdin_lock, irq);
            if (copy_to_user((void *)argp, &avail, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TIOCEXCL:
        case TIOCNXCL:
            // Exclusive mode, no-op
            frame->rax = 0;
            return;

        case TIOCSCTTY: {
            int tidx = -1;
            if (entry) {
                if (entry->type == FD_STREAM) {
                    tidx = 0;
                } else if (entry->type == FD_DEV) {
                    char rel[256];
                    if (match_vfs_path(entry->path, "devtmpfs", rel)) {
                        tidx = tty_rel_to_idx(rel);
                        if (tidx < 0 && strncmp(rel, "pts/", 4) == 0) {
                            int pidx = pty_rel_to_idx(rel + 4);
                            if (pidx >= 0) tidx = 100 + pidx;
                        }
                    } else if (match_vfs_path(entry->path, "devpts", rel)) {
                        int pidx = pty_rel_to_idx(rel);
                        if (pidx >= 0) tidx = 100 + pidx;
                    }
                }
            }
            if (tidx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (tidx >= 100) {
                pty_t *pty_ct = get_pty(tidx - 100);
                if (!pty_ct || !pty_ct->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                current_task_ptr->ctty_idx = tidx;
                pty_ct->fg_pgrp = current_task_ptr->pgid;
            } else {
                tty_t *tty_ct = get_tty(tidx);
                if (!tty_ct) { frame->rax = (uint64_t)-ENOTTY; return; }
                current_task_ptr->ctty_idx = tidx;
                tty_ct->fg_pgrp = current_task_ptr->pgid;
                // Drop any keystrokes that arrived on this TTY before a
                // reader existed (e.g. a key held during boot).  PS/2
                // keyboards auto-repeat in hardware, so a single held key
                // can flood the ring with hundreds of bytes; without this
                // flush they'd be read first and wedge the input parser.
                uint64_t irq;
                spin_lock_irqsave(&tty_lock, &irq);
                tty_ct->input.head = tty_ct->input.tail = 0;
                spin_unlock_irqrestore(&tty_lock, irq);
            }
            frame->rax = 0;
            return;
        }

        case TIOCNOTTY:
            current_task_ptr->ctty_idx = -1;
            frame->rax = 0;
            return;

        case TCGETS2: {
            struct termios t = {0};
            struct termios2 t2 = {0};
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) {
                frame->rax = (uint64_t)-ENOTTY; return;
            }
            if (idx >= 100) {
                pty_t *pty_tcgets = get_pty(idx - 100);
                if (!pty_tcgets || !pty_tcgets->allocated) {
                    frame->rax = (uint64_t)-ENOTTY; return;
                }
                t = pty_tcgets->termios;
            } else {
                tty_t *tty_tcgets = get_tty(idx);
                if (!tty_tcgets) {
                    frame->rax = (uint64_t)-ENOTTY; return;
                }
                t = tty_tcgets->termios;
            }

            t2.c_iflag = t.c_iflag;
            t2.c_oflag = t.c_oflag;
            t2.c_cflag = t.c_cflag;
            t2.c_lflag = t.c_lflag;
            t2.c_line = t.c_line;
            for (int i = 0; i < NCCS2; i++) t2.c_cc[i] = t.c_cc[i];
            t2.c_ispeed = t.c_ispeed;
            t2.c_ospeed = t.c_ospeed;

            if (copy_to_user((void *)argp, &t2, sizeof(t2)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2: {
            struct termios2 t2;
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (copy_from_user(&t2, (const void *)argp, sizeof(t2)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (idx >= 100) {
                pty_t *pty_tcsets = get_pty(idx - 100);
                if (!pty_tcsets || !pty_tcsets->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                pty_tcsets->termios.c_iflag = t2.c_iflag;
                pty_tcsets->termios.c_oflag = t2.c_oflag;
                pty_tcsets->termios.c_cflag = t2.c_cflag;
                pty_tcsets->termios.c_lflag = t2.c_lflag;
                pty_tcsets->termios.c_line = t2.c_line;
                for (int i = 0; i < NCCS2; i++) pty_tcsets->termios.c_cc[i] = t2.c_cc[i];
                pty_tcsets->termios.c_ispeed = t2.c_ispeed;
                pty_tcsets->termios.c_ospeed = t2.c_ospeed;
            } else {
                tty_t *tty_tcsets = get_tty(idx);
                if (!tty_tcsets) { frame->rax = (uint64_t)-ENOTTY; return; }
                tty_tcsets->termios.c_iflag = t2.c_iflag;
                tty_tcsets->termios.c_oflag = t2.c_oflag;
                tty_tcsets->termios.c_cflag = t2.c_cflag;
                tty_tcsets->termios.c_lflag = t2.c_lflag;
                tty_tcsets->termios.c_line = t2.c_line;
                for (int i = 0; i < NCCS2; i++) tty_tcsets->termios.c_cc[i] = t2.c_cc[i];
                tty_tcsets->termios.c_ispeed = t2.c_ispeed;
                tty_tcsets->termios.c_ospeed = t2.c_ospeed;
            }
            frame->rax = 0;
            return;
        }

        case TIOCSPTLCK: {
            if (!entry || entry->type != FD_PTY_MASTER) { frame->rax = (uint64_t)-ENOTTY; return; }
            int idx = ptm_path_idx(entry->path);
            pty_t *p = get_pty(idx);
            if (!p) { frame->rax = (uint64_t)-EBADF; return; }
            int val = 0;
            if (copy_from_user(&val, (const void *)argp, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            p->locked = (val != 0);
            frame->rax = 0;
            return;
        }

        case TIOCGPTN: {
            if (!entry || entry->type != FD_PTY_MASTER) { frame->rax = (uint64_t)-ENOTTY; return; }
            int idx = ptm_path_idx(entry->path);
            unsigned int uidx = (unsigned int)idx;
            if (copy_to_user((void *)argp, &uidx, sizeof(unsigned int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        default:
            (void)is_tty;
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_pread64(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint8_t *buf = (uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;
    uint64_t offset = frame->r10;

    if (count > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (!fd_allows_read(entry)) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type == FD_EXT4) {
        if (count == 0) { frame->rax = 0; return; }
        if (count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t got = read_ext4(entry->path, kbuf, count, offset);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
        free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }
    if (entry->type != FD_FILE) { frame->rax = (uint64_t)-ESPIPE; return; }

    initrd_file_t file = read_initrd(entry->path);
    if (!file.data || offset >= file.size) {
        frame->rax = 0; return;
    }

    uint64_t avail = file.size - offset;
    uint64_t to_read = (count < avail) ? count : avail;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, (uint8_t *)file.data + offset, to_read) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = to_read;
}

void sys_readv(syscall_frame_t *frame) {
    const struct iovec *uiov = (const struct iovec *)frame->rsi;
    int iovcnt = (int)frame->rdx;

    if (iovcnt < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (iovcnt == 0) { frame->rax = 0; return; }
    if (!uiov) { frame->rax = (uint64_t)-EFAULT; return; }
    if (iovcnt > MAX_IOV) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)uiov, (uint64_t)iovcnt * sizeof(struct iovec))) { frame->rax = (uint64_t)-EFAULT; return; }

    struct iovec kiov[MAX_IOV];
    if (read_vmm(current_task_ptr->ctx, kiov, (uint64_t)uiov, (uint64_t)iovcnt * sizeof(struct iovec)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!kiov[i].iov_base) continue;
        if (kiov[i].iov_len == 0) continue;

        // Hand this segment off to the regular read path by spoofing rsi/rdx.
        uint64_t saved_rsi = frame->rsi;
        uint64_t saved_rdx = frame->rdx;
        frame->rsi = (uint64_t)kiov[i].iov_base;
        frame->rdx = kiov[i].iov_len;
        sys_read(frame); // Depend on sys_read() cuz why the fuck not.
        frame->rsi = saved_rsi;
        frame->rdx = saved_rdx;

        int64_t got = (int64_t)frame->rax;
        if (got < 0) {
            // Pass the error up, but report bytes already read (if any) per POSIX.
            if (total > 0) { frame->rax = total; return; }
            return;
        }

        total += (uint64_t)got;
        if ((uint64_t)got < kiov[i].iov_len) break; // short read: stop here
    }

    frame->rax = total;
}

void sys_writev(syscall_frame_t *frame) {
    const struct iovec *uiov = (const struct iovec *)frame->rsi;
    int iovcnt = (int)frame->rdx;

    if (iovcnt < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (iovcnt == 0) { frame->rax = 0; return; }
    if (!uiov) { frame->rax = (uint64_t)-EFAULT; return; }
    if (iovcnt > MAX_IOV) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)uiov, (uint64_t)iovcnt * sizeof(struct iovec))) { frame->rax = (uint64_t)-EFAULT; return; }

    struct iovec kiov[MAX_IOV];
    if (read_vmm(current_task_ptr->ctx, kiov, (uint64_t)uiov, (uint64_t)iovcnt * sizeof(struct iovec)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!kiov[i].iov_base) continue;
        if (kiov[i].iov_len == 0) continue;

        uint64_t saved_rsi = frame->rsi;
        uint64_t saved_rdx = frame->rdx;
        frame->rsi = (uint64_t)kiov[i].iov_base;
        frame->rdx = kiov[i].iov_len;
        sys_write(frame); // Depend on sys_write() cuz why the fuck not.
        frame->rsi = saved_rsi;
        frame->rdx = saved_rdx;

        int64_t wrote = (int64_t)frame->rax;
        if (wrote < 0) {
            if (total > 0) { frame->rax = total; return; }
            return;
        }

        total += (uint64_t)wrote;
        if ((uint64_t)wrote < kiov[i].iov_len) break; // short write: stop here
    }

    frame->rax = total;
}

static int check_path_access(const char *path, const char *abs_path, int mode, bool follow) {
    int want_read = (mode & R_OK) != 0;
    int want_write = (mode & W_OK) != 0;
    int want_exec = (mode & X_OK) != 0;

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst) || stat_tmpfs_to_kst(abs_path, &kst, follow) || stat_ext4_to_kst(abs_path, &kst, follow) || stat_proc(abs_path, path, &kst, follow)) {
        if (want_write && check_ext4_path(abs_path)) return -EROFS;
        return mode == F_OK || can_access_stat_mode(&kst, want_read, want_write, want_exec) ? 0 : -EACCES;
    }

    initrd_file_t file = follow ? stat_initrd(abs_path) : stat_initrd_nofollow(abs_path);
    if (!file.mode) return -ENOENT;
    return mode == F_OK || can_access_initrd(&file, want_read, want_write, want_exec) ? 0 : -EACCES;
}

static int check_access_at(int dirfd, const char *user_path, int mode, int flags) {
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

void sys_access(syscall_frame_t *frame) {
    frame->rax = (uint64_t)check_access_at(AT_FDCWD, (const char *)frame->rdi, (int)frame->rsi, AT_EACCESS);
}

void sys_faccessat(syscall_frame_t *frame) {
    frame->rax = (uint64_t)check_access_at((int)frame->rdi, (const char *)frame->rsi, (int)frame->rdx, AT_EACCESS);
}

void sys_faccessat2(syscall_frame_t *frame) {
    frame->rax = (uint64_t)check_access_at((int)frame->rdi, (const char *)frame->rsi, (int)frame->rdx, (int)frame->r10);
}

void sys_pipe(syscall_frame_t *frame) {
    int *pipefd = (int *)frame->rdi;
    unix_handle_t *read_end = NULL;
    unix_handle_t *write_end = NULL;
    int fds[2];
    int r;

    if (!pipefd) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)pipefd, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    r = create_unix_pipe(&read_end, &write_end);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:r", FD_PIPE, O_RDONLY, read_end);
    if (fds[0] < 0) {
        release_unix_handle(read_end);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[0];
        return;
    }
    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:w", FD_PIPE, O_WRONLY, write_end);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)pipefd, fds, sizeof(fds)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_select(syscall_frame_t *frame) {
    int nfds = (int)frame->rdi;
    uint64_t *readfds   = (uint64_t *)frame->rsi;
    uint64_t *writefds  = (uint64_t *)frame->rdx;
    uint64_t *exceptfds = (uint64_t *)frame->r10;
    struct timeval *timeout_ptr = (struct timeval *)frame->r8;

    if (nfds < 0 || nfds > FD_SETSIZE) { frame->rax = (uint64_t)-EINVAL; return; }

    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timeval))) { frame->rax = (uint64_t)-EFAULT; return; }
    }
    if (nfds > 0) {
        int bytes = ((nfds + 63) / 64) * 8;
        if (readfds   && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)readfds,   bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (writefds  && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)writefds,  bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (exceptfds && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)exceptfds, bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    int64_t timeout_us = -1;
    if (timeout_ptr) {
        struct timeval tv;
        uint64_t converted;
        if (copy_from_user(&tv, timeout_ptr, sizeof(tv)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (timeval_to_us(&tv, &converted) < 0 || converted > INT64_MAX) { frame->rax = (uint64_t)-EINVAL; return; }
        timeout_us = (int64_t)converted;
    } else {
        timeout_us = -1; // infinite
    }

    int set_bytes = nfds > 0 ? ((nfds + 7) / 8) : 0;
    int qword_bytes = nfds > 0 ? ((nfds + 63) / 64) * 8 : 0;

    uint8_t *k_read = NULL, *k_write = NULL, *k_except = NULL;
    uint8_t *o_read = NULL, *o_write = NULL, *o_except = NULL;

    if (set_bytes > 0) {
        k_read   = malloc(qword_bytes);
        k_write  = malloc(qword_bytes);
        k_except = malloc(qword_bytes);
        o_read   = malloc(qword_bytes);
        o_write  = malloc(qword_bytes);
        o_except = malloc(qword_bytes);
        if (!k_read || !k_write || !k_except || !o_read || !o_write || !o_except) {
            free(k_read); free(k_write); free(k_except);
            free(o_read); free(o_write); free(o_except);
            frame->rax = (uint64_t)-ENOMEM; return;
        }
        memset(k_read,   0, qword_bytes);
        memset(k_write,  0, qword_bytes);
        memset(k_except, 0, qword_bytes);
        memset(o_read,   0, qword_bytes);
        memset(o_write,  0, qword_bytes);
        memset(o_except, 0, qword_bytes);
        if (readfds)   copy_from_user(k_read,   readfds,   set_bytes);
        if (writefds)  copy_from_user(k_write,  writefds,  set_bytes);
        if (exceptfds) copy_from_user(k_except, exceptfds, set_bytes);
    }

    int64_t ret = do_select(nfds, k_read, k_write, k_except, o_read, o_write, o_except, qword_bytes, timeout_us);

    if (ret >= 0) {
        if (readfds   && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)readfds,   o_read,   set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && writefds  && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)writefds,  o_write,  set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && exceptfds && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)exceptfds, o_except, set_bytes) < 0) ret = -EFAULT;
    }

    free(k_read); free(k_write); free(k_except);
    free(o_read); free(o_write); free(o_except);

    frame->rax = (uint64_t)ret;
}

void sys_sched_yield(syscall_frame_t *frame) {
    spin_unlock(&sched_lock);
    yield_sched();
    spin_lock(&sched_lock);
    frame->rax = 0;
}

void sys_dup(syscall_frame_t *frame) {
    int oldfd = (int)frame->rdi;

    fd_entry_t *src = get_current_fd(oldfd);
    if (!src) { frame->rax = (uint64_t)-EBADF; return; }

    // Find the lowest free fd
    fd_table_t *table = &current_task_ptr->fd_table;
    for (int i = 0; i < FD_MAX; i++) {
        if (!table->entries[i].open) {
            table->entries[i] = *src;   // copy full entry
            table->entries[i].open = true;
            retain_fd_entry(&table->entries[i]);
            frame->rax = (uint64_t)i;
            return;
        }
    }
    frame->rax = (uint64_t)-EMFILE;
}

void sys_dup2(syscall_frame_t *frame) {
    int oldfd = (int)frame->rdi;
    int newfd = (int)frame->rsi;

    if (newfd < 0 || newfd >= FD_MAX) { frame->rax = (uint64_t)-EBADF; return; }

    fd_entry_t *src = get_current_fd(oldfd);
    if (!src) { frame->rax = (uint64_t)-EBADF; return; }

    if (oldfd == newfd) { frame->rax = (uint64_t)newfd; return; }

    fd_table_t *table = &current_task_ptr->fd_table;

    // Close newfd if it's already open
    if (table->entries[newfd].open) free_fd(table, newfd);

    table->entries[newfd] = *src;
    table->entries[newfd].open = true;
    retain_fd_entry(&table->entries[newfd]);
    frame->rax = (uint64_t)newfd;
}

void sys_nanosleep(syscall_frame_t *frame) {
    frame->rax = (uint64_t)do_clock_nanosleep(CLOCK_MONOTONIC, 0, (const struct timespec *)frame->rdi, (struct timespec *)frame->rsi);
}

void sys_clock_nanosleep(syscall_frame_t *frame) {
    frame->rax = (uint64_t)do_clock_nanosleep((int)frame->rdi, (int)frame->rsi, (const struct timespec *)frame->rdx, (struct timespec *)frame->r10);
}

void sys_getitimer(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    struct itimerval *user_value = (struct itimerval *)frame->rsi;
    struct itimerval value;

    if (which != ITIMER_REAL) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_value || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_value, sizeof(value))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    update_interval_timers();
    fill_real_itimer(current_task_ptr, &value);
    copy_to_user(user_value, &value, sizeof(value));
    frame->rax = 0;
}

void sys_setitimer(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    const struct itimerval *user_new = (const struct itimerval *)frame->rsi;
    struct itimerval *user_old = (struct itimerval *)frame->rdx;
    struct itimerval new_value;
    struct itimerval old_value;
    uint64_t value_us;
    uint64_t interval_us;
    uint64_t deadline_us = 0;

    if (which != ITIMER_REAL) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_new || copy_from_user(&new_value, user_new, sizeof(new_value)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (user_old && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_old, sizeof(old_value))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (timeval_to_us(&new_value.it_value, &value_us) < 0 || timeval_to_us(&new_value.it_interval, &interval_us) < 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    update_interval_timers();
    fill_real_itimer(current_task_ptr, &old_value);

    if (value_us != 0) {
        uint64_t now = time_get_realtime_us();
        if (value_us > UINT64_MAX - now) { frame->rax = (uint64_t)-EINVAL; return; }
        deadline_us = now + value_us;
    }
    current_task_ptr->real_timer_interval_us = interval_us;
    current_task_ptr->real_timer_deadline_us = deadline_us;

    if (user_old) copy_to_user(user_old, &old_value, sizeof(old_value));
    frame->rax = 0;
}

void sys_getpid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_getpriority(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    id_t who = (id_t)frame->rsi;
    if (which != PRIO_PROCESS) { frame->rax = (uint64_t)-EINVAL; return; }
    task_t *task = find_priority_task(which, who);
    if (!task) { frame->rax = (uint64_t)-ESRCH; return; }
    frame->rax = (uint64_t)(20 - get_task_nice(task));
}

void sys_setpriority(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    id_t who = (id_t)frame->rsi;
    int nice = (int)frame->rdx;
    if (which != PRIO_PROCESS) { frame->rax = (uint64_t)-EINVAL; return; }
    task_t *task = find_priority_task(which, who);
    if (!task) { frame->rax = (uint64_t)-ESRCH; return; }
    if (current_task_ptr->euid != 0 && current_task_ptr->euid != task->euid && current_task_ptr->euid != task->uid) { frame->rax = (uint64_t)-EPERM; return; }
    if (current_task_ptr->euid != 0 && nice < get_task_nice(task)) { frame->rax = (uint64_t)-EACCES; return; }
    frame->rax = (uint64_t)set_task_nice(task, nice);
}

void sys_sendfile(syscall_frame_t *frame) {
    int out_fd = (int)frame->rdi;
    int in_fd = (int)frame->rsi;
    int64_t *offset_ptr = (int64_t *)frame->rdx;
    size_t count = (size_t)frame->r10;

    fd_entry_t *in_entry = get_current_fd(in_fd);
    if (!in_entry) { frame->rax = (uint64_t)-EBADF; return; }
    fd_entry_t *out_entry = get_current_fd(out_fd);
    if (!out_entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (!fd_allows_read(in_entry) || !fd_allows_write(out_entry)) {
        frame->rax = (uint64_t)-EBADF;
        return;
    }

    // Only support file-to-file for now
    if (in_entry->type != FD_FILE || out_entry->type != FD_FILE) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    initrd_file_t in_file = read_initrd(in_entry->path);
    if (!in_file.data) { frame->rax = (uint64_t)-EBADF; return; }
    if (!can_access_initrd(&in_file, 1, 0, 0)) { frame->rax = (uint64_t)-EACCES; return; }

    initrd_file_t out_file = read_initrd(out_entry->path);
    if (!out_file.mode || !can_access_initrd(&out_file, 0, 1, 0)) {
        frame->rax = (uint64_t)-EACCES;
        return;
    }

    int64_t offset = 0;
    if (offset_ptr) {
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)offset_ptr, sizeof(int64_t))) { frame->rax = (uint64_t)-EFAULT; return; }
        if (read_vmm(current_task_ptr->ctx, &offset, (uint64_t)offset_ptr, sizeof(int64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    } else {
        offset = (int64_t)in_entry->offset;
    }

    if (offset < 0 || (uint64_t)offset > in_file.size) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t avail = in_file.size - (uint64_t)offset;
    uint64_t to_copy = (count < avail) ? count : avail;

    if (to_copy == 0) { frame->rax = 0; return; }

    // Read source data from initrd
    if (out_entry->offset > UINT64_MAX - to_copy) { frame->rax = (uint64_t)-EFBIG; return; }
    uint64_t new_size = out_entry->offset + to_copy;
    if (out_file.size > new_size) new_size = out_file.size;
    if (new_size > INITRD_MAX_FILE_SIZE) { frame->rax = (uint64_t)-EFBIG; return; }

    void *new_data = malloc(new_size);
    if (!new_data) { frame->rax = (uint64_t)-ENOMEM; return; }

    memset(new_data, 0, new_size);
    if (out_file.data && out_file.size)
        memcpy(new_data, out_file.data, out_file.size);
    memcpy((uint8_t *)new_data + out_entry->offset, (uint8_t *)in_file.data + (uint64_t)offset, to_copy);

    int res = write_initrd(out_entry->path, new_data, new_size, out_file.mode ? out_file.mode : 0644, out_file.mode ? out_file.uid : current_task_ptr->euid, out_file.mode ? out_file.gid : current_task_ptr->egid);
    free(new_data);

    if (res < 0) { frame->rax = (uint64_t)res; return; }

    out_entry->offset += to_copy;
    offset += (int64_t)to_copy;

    if (offset_ptr) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)offset_ptr, &offset, sizeof(int64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    } else {
        in_entry->offset = (uint64_t)offset;
    }

    frame->rax = to_copy;
}


void sys_socket(syscall_frame_t *frame) {
    int domain = (int)frame->rdi;
    int type = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int socket_flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int base_type = type & SOCK_TYPE_MASK;
    socket_t *sock = NULL;

    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if ((domain == AF_PACKET || (domain == AF_INET && base_type == SOCK_RAW)) && (!current_task_ptr || current_task_ptr->euid != 0)) {
        frame->rax = (uint64_t)-EPERM;
        return;
    }

    int r = create_socket(domain, base_type, protocol, &sock);
    if (r < 0) { frame->rax = (uint64_t)r; return; }
    sock->flags = socket_flags & SOCK_NONBLOCK;

    int fd = alloc_fd_handle(&current_task_ptr->fd_table, "socket", FD_SOCKET, O_RDWR | (socket_flags & SOCK_NONBLOCK), sock);
    if (fd < 0) {
        release_socket(sock);
        frame->rax = (uint64_t)fd;
        return;
    }
    frame->rax = (uint64_t)fd;
}

void sys_connect(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *addr = (const void *)frame->rsi;
    uint32_t addrlen = (uint32_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (addrlen > 128) { frame->rax = (uint64_t)-EINVAL; return; }
    if (addrlen > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
    uint8_t kaddr[128];
    memset(kaddr, 0, sizeof(kaddr));
    uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
    if (read_vmm(current_task_ptr->ctx, kaddr, (uint64_t)addr, copy_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    int access = prepare_unix_socket_path(kaddr, &copy_len, false);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->connect) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->connect(sock, kaddr, copy_len);
}

void sys_accept(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    fd_entry_t *entry = get_current_fd(fd);
    socket_t *accepted = NULL;
    int r;
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->accept) { frame->rax = (uint64_t)-EINVAL; return; }

    r = sock->ops->accept(sock, &accepted);
    if (r < 0) { frame->rax = (uint64_t)r; return; }
    int newfd = alloc_fd_handle(&current_task_ptr->fd_table, "socket:accepted", FD_SOCKET, O_RDWR, accepted);
    if (newfd < 0) {
        release_socket(accepted);
        frame->rax = (uint64_t)newfd;
        return;
    }
    frame->rax = (uint64_t)newfd;
}

void sys_sendto(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    const void *dest_addr = (const void *)frame->r8;
    socklen_t addrlen = (socklen_t)frame->r9;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (len > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
    if (addrlen > 128) { frame->rax = (uint64_t)-EINVAL; return; }
    if (len > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)buf, len)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint8_t kaddr[128];
    if (dest_addr && addrlen > 0) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)dest_addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
        uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
        if (read_vmm(current_task_ptr->ctx, kaddr, (uint64_t)dest_addr, copy_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    uint8_t *kbuf = malloc(len);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
    if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, len) < 0) { free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }

    socket_t *sock = (socket_t *)entry->handle;
    int64_t w = -EBADF;
    if (sock && sock->ops && sock->ops->sendto) {
        w = sock->ops->sendto(sock, kbuf, len, flags, (dest_addr && addrlen > 0) ? kaddr : NULL, dest_addr ? addrlen : 0);
    }
    free(kbuf);
    frame->rax = (uint64_t)w;
}

void sys_recvfrom(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    void *src_addr = (void *)frame->r8;
    socklen_t *addrlen_ptr = (socklen_t *)frame->r9;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (len > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
    if (len > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, len)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint8_t kaddr[128] = {0};
    socklen_t user_addrlen = 0;
    socklen_t kaddrlen = 0;
    if (src_addr || addrlen_ptr) {
        if (!src_addr || !addrlen_ptr || copy_from_user(&user_addrlen, addrlen_ptr, sizeof(user_addrlen)) < 0) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
        kaddrlen = user_addrlen < sizeof(kaddr) ? user_addrlen : sizeof(kaddr);
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)src_addr, kaddrlen) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)addrlen_ptr, sizeof(user_addrlen))) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
    }
    uint8_t *kbuf = malloc(len);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }

    socket_t *sock = (socket_t *)entry->handle;
    int64_t got = -EBADF;
    if (sock && sock->ops && sock->ops->recvfrom) {
        got = sock->ops->recvfrom(sock, kbuf, len, flags, kaddr, &kaddrlen);
    }
    if (got > (int64_t)len) {
        got = -EIO;
    }
    if (got >= 0) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (size_t)got) < 0) { free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
        if (src_addr && addrlen_ptr) {
            socklen_t copy_len = kaddrlen < user_addrlen ? kaddrlen : user_addrlen;
            if (copy_len > sizeof(kaddr)) copy_len = sizeof(kaddr);
            if (copy_len) (void)write_vmm(current_task_ptr->ctx, (uint64_t)src_addr, kaddr, copy_len);
            (void)write_vmm(current_task_ptr->ctx, (uint64_t)addrlen_ptr, &kaddrlen, sizeof(socklen_t));
        }
    }
    free(kbuf);
    frame->rax = (uint64_t)got;
}

void sys_sendmsg(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const struct msghdr *user_msg = (const struct msghdr *)frame->rsi;
    int flags = (int)frame->rdx;
    struct msghdr msg;
    struct iovec *iov = NULL;
    uint8_t *buf = NULL;
    uint8_t name[128];
    size_t total = 0;
    int64_t result = -EBADF;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (copy_from_user(&msg, user_msg, sizeof(msg)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (msg.msg_iovlen > MAX_IOV || msg.msg_namelen > sizeof(name)) { frame->rax = (uint64_t)-EINVAL; return; }
    if (msg.msg_iovlen) {
        size_t iov_size = msg.msg_iovlen * sizeof(*iov);
        iov = malloc(iov_size);
        if (!iov) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (copy_from_user(iov, msg.msg_iov, iov_size) < 0) { result = -EFAULT; goto out; }
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (iov[i].iov_len > MAX_IO_COUNT - total || (iov[i].iov_len && !user_range_ok(current_task_ptr->ctx, (uint64_t)iov[i].iov_base, iov[i].iov_len))) {
                result = -EFAULT;
                goto out;
            }
            total += iov[i].iov_len;
        }
    }
    if (msg.msg_name && msg.msg_namelen && copy_from_user(name, msg.msg_name, msg.msg_namelen) < 0) {
        result = -EFAULT;
        goto out;
    }

    buf = malloc(total ? total : 1);
    if (!buf) { result = -ENOMEM; goto out; }
    size_t offset = 0;
    for (size_t i = 0; i < msg.msg_iovlen; i++) {
        if (iov[i].iov_len && copy_from_user(buf + offset, iov[i].iov_base, iov[i].iov_len) < 0) {
            result = -EFAULT;
            goto out;
        }
        offset += iov[i].iov_len;
    }

    socket_t *sock = (socket_t *)entry->handle;
    if (sock && sock->ops && sock->ops->sendto) {
        result = sock->ops->sendto(sock, buf, total, flags, msg.msg_name ? name : NULL, msg.msg_namelen);
    }

out:
    if (buf) free(buf);
    if (iov) free(iov);
    frame->rax = (uint64_t)result;
}

void sys_recvmsg(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    struct msghdr *user_msg = (struct msghdr *)frame->rsi;
    int flags = (int)frame->rdx;
    struct msghdr msg;
    struct iovec *iov = NULL;
    uint8_t *buf = NULL;
    uint8_t name[128] = {0};
    socklen_t name_len;
    size_t total = 0;
    int64_t result = -EBADF;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (copy_from_user(&msg, user_msg, sizeof(msg)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (msg.msg_iovlen > MAX_IOV || msg.msg_namelen > sizeof(name)) { frame->rax = (uint64_t)-EINVAL; return; }
    if (msg.msg_name && msg.msg_namelen && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)msg.msg_name, msg.msg_namelen)) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    if (msg.msg_iovlen) {
        size_t iov_size = msg.msg_iovlen * sizeof(*iov);
        iov = malloc(iov_size);
        if (!iov) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (copy_from_user(iov, msg.msg_iov, iov_size) < 0) { result = -EFAULT; goto out; }
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (iov[i].iov_len > MAX_IO_COUNT - total || (iov[i].iov_len && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)iov[i].iov_base, iov[i].iov_len))) {
                result = -EFAULT;
                goto out;
            }
            total += iov[i].iov_len;
        }
    }

    buf = malloc(total ? total : 1);
    if (!buf) { result = -ENOMEM; goto out; }
    name_len = msg.msg_name ? msg.msg_namelen : 0;
    socket_t *sock = (socket_t *)entry->handle;
    if (sock && sock->ops && sock->ops->recvfrom) {
        result = sock->ops->recvfrom(sock, buf, total, flags, msg.msg_name ? name : NULL, msg.msg_name ? &name_len : NULL);
    }
    if (result < 0) goto out;
    if ((uint64_t)result > total) { result = -EIO; goto out; }

    size_t remaining = (size_t)result;
    size_t offset = 0;
    for (size_t i = 0; i < msg.msg_iovlen && remaining; i++) {
        size_t copy_len = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
        if (copy_to_user(iov[i].iov_base, buf + offset, copy_len) < 0) { result = -EFAULT; goto out; }
        offset += copy_len;
        remaining -= copy_len;
    }
    if (msg.msg_name && name_len) {
        socklen_t copy_len = name_len < msg.msg_namelen ? name_len : msg.msg_namelen;
        if (copy_len > sizeof(name)) copy_len = sizeof(name);
        if (copy_len && copy_to_user(msg.msg_name, name, copy_len) < 0) { result = -EFAULT; goto out; }
    }
    msg.msg_namelen = name_len;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    if (copy_to_user(user_msg, &msg, sizeof(msg)) < 0) result = -EFAULT;

out:
    if (buf) free(buf);
    if (iov) free(iov);
    frame->rax = (uint64_t)result;
}

void sys_shutdown(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int how = (int)frame->rsi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->shutdown) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->shutdown(sock, how);
}

void sys_bind(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *addr = (const void *)frame->rsi;
    uint32_t addrlen = (uint32_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (addrlen > 128) { frame->rax = (uint64_t)-EINVAL; return; }
    if (addrlen > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
    uint8_t kaddr[128];
    memset(kaddr, 0, sizeof(kaddr));
    uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
    if (read_vmm(current_task_ptr->ctx, kaddr, (uint64_t)addr, copy_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    int access = prepare_unix_socket_path(kaddr, &copy_len, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->bind) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->bind(sock, kaddr, copy_len);
}

void sys_listen(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int backlog = (int)frame->rsi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->listen) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->listen(sock, backlog);
}

void sys_getsockname(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *user_addr = (void *)frame->rsi;
    socklen_t *user_addrlen = (socklen_t *)frame->rdx;
    uint8_t addr[128];
    socklen_t addrlen;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (!user_addr || !user_addrlen || copy_from_user(&addrlen, user_addrlen, sizeof(addrlen)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (addrlen > sizeof(addr)) addrlen = sizeof(addr);
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addr, addrlen) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addrlen, sizeof(addrlen))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->getsockname) { frame->rax = (uint64_t)-EOPNOTSUPP; return; }
    int result = sock->ops->getsockname(sock, addr, &addrlen);
    if (result < 0) { frame->rax = (uint64_t)result; return; }
    if (addrlen > sizeof(addr)) { frame->rax = (uint64_t)-EIO; return; }

    if (copy_to_user(user_addr, addr, addrlen) < 0 || copy_to_user(user_addrlen, &addrlen, sizeof(addrlen)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_getpeername(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *user_addr = (void *)frame->rsi;
    socklen_t *user_addrlen = (socklen_t *)frame->rdx;
    uint8_t addr[128];
    socklen_t addrlen;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (!user_addr || !user_addrlen || copy_from_user(&addrlen, user_addrlen, sizeof(addrlen)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (addrlen > sizeof(addr)) addrlen = sizeof(addr);
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addr, addrlen) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addrlen, sizeof(addrlen))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->getpeername) { frame->rax = (uint64_t)-EOPNOTSUPP; return; }
    int result = sock->ops->getpeername(sock, addr, &addrlen);
    if (result < 0) { frame->rax = (uint64_t)result; return; }
    if (addrlen > sizeof(addr)) { frame->rax = (uint64_t)-EIO; return; }

    if (copy_to_user(user_addr, addr, addrlen) < 0 || copy_to_user(user_addrlen, &addrlen, sizeof(addrlen)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_socketpair(syscall_frame_t *frame) {
    int domain = (int)frame->rdi;
    int type = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int *sv = (int *)frame->r10;
    socket_t *a = NULL;
    socket_t *b = NULL;
    int fds[2];
    int r;

    if (!sv) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)sv, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    r = create_socketpair(domain, type, protocol, &a, &b);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "socketpair", FD_SOCKET, O_RDWR, a);
    if (fds[0] < 0) {
        release_socket(a);
        release_socket(b);
        frame->rax = (uint64_t)fds[0];
        return;
    }
    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "socketpair", FD_SOCKET, O_RDWR, b);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_socket(b);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)sv, fds, sizeof(fds)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_setsockopt(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (level != SOL_SOCKET) { frame->rax = (uint64_t)-ENOPROTOOPT; return; }
    switch (optname) {
        case SO_REUSEADDR:
        case SO_KEEPALIVE:
        case SO_BROADCAST:
        case SO_LINGER:
            frame->rax = 0;
            return;
        case SO_BINDTODEVICE: {
            const char *name = (const char *)frame->r10;
            socklen_t length = (socklen_t)frame->r8;
            char interface[IFNAMSIZ];
            if (!name || length == 0 || length > IFNAMSIZ) { frame->rax = (uint64_t)-EINVAL; return; }
            memset(interface, 0, sizeof(interface));
            if (copy_from_user(interface, name, length) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            interface[IFNAMSIZ - 1] = '\0';
            if (strcmp(interface, "eth0") != 0) { frame->rax = (uint64_t)-ENODEV; return; }
            frame->rax = 0;
            return;
        }
        default:
            frame->rax = (uint64_t)-ENOPROTOOPT;
            return;
    }
}

void sys_getsockopt(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    int *optval = (int *)frame->r10;
    uint32_t *optlen = (uint32_t *)frame->r8;
    fd_entry_t *entry = get_current_fd(fd);
    int val;

    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (level != SOL_SOCKET) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)optval, sizeof(int)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)optlen, sizeof(uint32_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (optname == SO_ERROR) val = get_unix_socket_error((unix_handle_t *)entry->handle);
    else if (optname == SO_TYPE) val = get_unix_socket_type((unix_handle_t *)entry->handle);
    else { frame->rax = (uint64_t)-ENOPROTOOPT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)optval, &val, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    uint32_t len = sizeof(int);
    if (write_vmm(current_task_ptr->ctx, (uint64_t)optlen, &len, sizeof(uint32_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}


void sys_clone(syscall_frame_t *frame) {
    uint64_t clone_flags = frame->rdi;
    uint64_t new_stack    = frame->rsi;
    int *parent_tidptr    = (int *)frame->rdx;
    int *child_tidptr     = (int *)frame->r10;
    uint64_t tls          = frame->r8;

    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    // Validate optional tid pointers if provided.
    if (parent_tidptr && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)parent_tidptr, sizeof(int))) { frame->rax = (uint64_t)-EFAULT; return; }
    if (child_tidptr  && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)child_tidptr, sizeof(int))) { frame->rax = (uint64_t)-EFAULT; return; }
    // CLONE_SETTLS: tls must be a valid user address.
    if ((clone_flags & CLONE_SETTLS) && tls != 0 && !user_address_range_ok(tls, 1)) { frame->rax = (uint64_t)-EPERM; return; }

    // CLONE_SIGHAND without CLONE_VM is not allowed (Linux returns EINVAL).
    if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM)) { frame->rax = (uint64_t)-EINVAL; return; }
    // CLONE_THREAD requires CLONE_SIGHAND (hence CLONE_VM).
    if ((clone_flags & CLONE_THREAD)  && !(clone_flags & CLONE_SIGHAND)) { frame->rax = (uint64_t)-EINVAL; return; }

    // Resolve the address space for the child.
    vmm_context_t *child_ctx;
    if (clone_flags & CLONE_VM) {
        // Share the parent's address space (threads).
        child_ctx = current_task_ptr->ctx;
        if (!retain_vmm_context(child_ctx)) { frame->rax = (uint64_t)-ENOMEM; return; }
    } else {
        // Copy-on-write style copy of the parent's address space (fork/clone).
        child_ctx = clone_vmm_context(current_task_ptr->ctx);
        if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }
    }

    pid_t child_pid = clone_task_flags(frame, child_ctx, clone_flags, new_stack, parent_tidptr, child_tidptr, tls);
    if (child_pid < 0) {
        destroy_vmm_context(child_ctx);
        frame->rax = (uint64_t)-EAGAIN;
        return;
    }

    frame->rax = (uint64_t)child_pid;

    if (clone_flags & CLONE_VFORK) {
        current_task_ptr->state       = TASK_STOPPED;
        current_task_ptr->waiting_for = child_pid;
        current_task_ptr->stopped_by_signal = 0;
        spin_unlock(&sched_lock);
        sti();
        yield_sched();
        spin_lock(&sched_lock);
        cli();
    }
}

void sys_fork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { destroy_vmm_context(child_ctx); frame->rax = (uint64_t)-EAGAIN; return; }

    frame->rax = (uint64_t)child_pid;
}

void sys_vfork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { destroy_vmm_context(child_ctx); frame->rax = (uint64_t)-EAGAIN; return; }

    current_task_ptr->state = TASK_STOPPED;
    current_task_ptr->waiting_for = child_pid;
    current_task_ptr->stopped_by_signal = 0;

    frame->rax = (uint64_t)child_pid;

    spin_unlock(&sched_lock);
    sti();
    yield_sched();
    spin_lock(&sched_lock);
    cli();
}

void sys_execve(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    char **user_argv = (char **)frame->rsi;
    char **user_envp = (char **)frame->rdx;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }
    char path_buf[256];

    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    // Check for shebang
    initrd_file_t probe = read_initrd(path_buf);
    if (probe.data && probe.size >= 2 && ((char *)probe.data)[0] == '#' && ((char *)probe.data)[1] == '!') {
        // Parse interpreter from shebang line
        char *p = (char *)probe.data + 2;
        while (*p == ' ' || *p == '\t') p++;

        char interp[256] = {0};
        int interp_len = 0;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' && interp_len < 255)
            interp[interp_len++] = *p++;
        interp[interp_len] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        char interp_arg[256] = {0};
        int interp_arg_len = 0;
        while (*p && *p != '\n' && *p != '\r' && interp_arg_len < 255)
            interp_arg[interp_arg_len++] = *p++;
        interp_arg[interp_arg_len] = '\0';

        if (interp_len > 0) {
            char **orig_argv = NULL;
            int orig_argc = copy_from_user_strarray(&orig_argv, (const char **)user_argv, 63);
            if (orig_argc < 0) { frame->rax = (uint64_t)orig_argc; return; }

            bool has_interp_arg = interp_arg_len > 0;
            int script_index = has_interp_arg ? 2 : 1;
            int new_argc = orig_argc + 1 + has_interp_arg;
            char **new_argv = vmalloc((new_argc + 1) * sizeof(char *));
            if (!new_argv) {
                free_strarray(orig_argv, orig_argc);
                frame->rax = (uint64_t)-ENOMEM; return;
            }

            new_argv[0] = malloc(interp_len + 1);
            if (!new_argv[0]) {
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)-ENOMEM; return;
            }
            memcpy(new_argv[0], interp, interp_len + 1);

            if (has_interp_arg) {
                new_argv[1] = malloc(interp_arg_len + 1);
                if (!new_argv[1]) {
                    free(new_argv[0]);
                    free_strarray(orig_argv, orig_argc);
                    vfree(new_argv);
                    frame->rax = (uint64_t)-ENOMEM; return;
                }
                memcpy(new_argv[1], interp_arg, interp_arg_len + 1);
            }

            new_argv[script_index] = malloc(strlen(path_buf) + 1);
            if (!new_argv[script_index]) {
                free(new_argv[0]);
                if (has_interp_arg) free(new_argv[1]);
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)-ENOMEM; return;
            }
            memcpy(new_argv[script_index], path_buf, strlen(path_buf) + 1);

            for (int i = 1; i < orig_argc; i++) new_argv[script_index + i] = orig_argv[i];
            new_argv[new_argc] = NULL;
            // Copy envp
            char **envp_ptrs = NULL;
            int envc = copy_from_user_strarray(&envp_ptrs, (const char **)user_envp, 63);
            if (envc < 0) {
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)envc; return;
            }
            int res = execve_elf(new_argv[0], new_argv, envp_ptrs, frame);
            if (res == 0) {
                current_task_ptr->fs_base = 0;
                current_task_ptr->gs_base = 0;
                write_msr(MSR_FS_BASE, 0);
                write_msr(MSR_KERNEL_GS_BASE, 0);
                for (int i = 1; i < 32; i++) {
                    uint64_t *sa = &current_task_ptr->sigactions[i * 4];
                    if (sa[0] != 0 && sa[0] != 1) {
                        sa[0] = 0; sa[1] = 0; sa[2] = 0; sa[3] = 0;
                    }
                }
                current_task_ptr->pending_signals = 0;
            }
            free(new_argv[0]);
            if (has_interp_arg) free(new_argv[1]);
            free(new_argv[script_index]);
            free_strarray(orig_argv, orig_argc);
            free_strarray(envp_ptrs, envc);
            vfree(new_argv);
            frame->rax = (uint64_t)res;
            return;
        }
    }

    // Normal (non-shebang) path
    char **argv_ptrs = NULL;
    char **envp_ptrs = NULL;
    int argc = copy_from_user_strarray(&argv_ptrs, (const char **)user_argv, 63);
    if (argc < 0) { frame->rax = (uint64_t)argc; return; }
    int envc = copy_from_user_strarray(&envp_ptrs, (const char **)user_envp, 63);
    if (envc < 0) {
        free_strarray(argv_ptrs, argc);
        frame->rax = (uint64_t)envc;
        return;
    }
    int res = execve_elf(path_buf, argv_ptrs, envp_ptrs, frame);
    if (res == 0) {
        current_task_ptr->fs_base = 0;
        current_task_ptr->gs_base = 0;
        write_msr(MSR_FS_BASE, 0);
        write_msr(MSR_KERNEL_GS_BASE, 0);
        for (int i = 1; i < 32; i++) {
            uint64_t *sa = &current_task_ptr->sigactions[i * 4];
            if (sa[0] != 0 && sa[0] != 1) {
                sa[0] = 0; sa[1] = 0; sa[2] = 0; sa[3] = 0;
            }
        }
        current_task_ptr->pending_signals = 0;
    }
    free_strarray(argv_ptrs, argc);
    free_strarray(envp_ptrs, envc);
    if (res != 0) frame->rax = (uint64_t)res;
}

void sys_exit(syscall_frame_t *frame) {
    int status = (int)frame->rdi;
    exit_task(status);
}

void sys_wait4(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int *wstatus = (int *)frame->rsi;
    int options = (int)frame->rdx;
    struct rusage *rusage = (struct rusage *)frame->r10;

    if (wstatus && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)wstatus, sizeof(*wstatus))) {
        frame->rax = (uint64_t)-EFAULT;
        spin_unlock(&sched_lock);
        return;
    }
    if (rusage && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)rusage, sizeof(*rusage))) {
        frame->rax = (uint64_t)-EFAULT;
        spin_unlock(&sched_lock);
        return;
    }

    while (1) {
        int found_child = 0;

        for (int i = 0; i < MAX_TASKS; i++) {
            // Only care about children of the current task
            if (!tasks[i]->state || tasks[i]->ppid != current_task_ptr->pid) continue;

            if (pid > 0 && tasks[i]->pid != pid) continue;
            if (pid == 0 && tasks[i]->pgid != current_task_ptr->pgid) continue;
            if (pid < -1 && tasks[i]->pgid != -pid) continue;

            found_child = 1;

            if (tasks[i]->state == TASK_ZOMBIE) {
                // Encode wstatus correctly (Linux wait status encoding)
                int status;
                if (tasks[i]->term_sig != 0) {
                    // Killed by signal: low 7 bits = signal number
                    status = tasks[i]->term_sig & 0x7f;
                } else {
                    // Normal exit: bits 8-15 = exit code, low byte = 0
                    status = (tasks[i]->exit_status & 0xff) << 8;
                }
                if (wstatus) {
                    if (write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int)) < 0) { release_task_slot(i); spin_unlock(&sched_lock); frame->rax = (uint64_t)-EFAULT; return; }
                }
                if (rusage) {
                    struct rusage ru = {0};
                    if (write_vmm(current_task_ptr->ctx, (uint64_t)rusage, &ru, sizeof(struct rusage)) < 0) { release_task_slot(i); spin_unlock(&sched_lock); frame->rax = (uint64_t)-EFAULT; return; }
                }
                pid_t ret = tasks[i]->pid;
                release_task_slot(i);
                frame->rax = (uint64_t)ret;
                
                spin_unlock(&sched_lock);
                return;
            }

            if ((options & WUNTRACED) && tasks[i]->state == TASK_STOPPED && tasks[i]->stopped_by_signal && !tasks[i]->stop_reported) {
                // Report stopped child (bits 8-15 = stop signal, low byte = 0x7f)
                int status = (SIGTSTP << 8) | 0x7f;
                if (wstatus) {
                    if (write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int)) < 0) { spin_unlock(&sched_lock); frame->rax = (uint64_t)-EFAULT; return; }
                }
                tasks[i]->stop_reported = 1;
                frame->rax = (uint64_t)tasks[i]->pid;
                
                spin_unlock(&sched_lock);
                return;
            }
        }

        // If we found no children at all, we can't wait
        if (!found_child) { 
            frame->rax = (uint64_t)-ECHILD; 
            spin_unlock(&sched_lock); // CRITICAL: Unlock before leaving!
            return; 
        }

        // WNOHANG: return 0 if no child has exited yet
        if (options & WNOHANG) { 
            frame->rax = 0; 
            spin_unlock(&sched_lock);
            return; 
        }

        // Return EINTR if any pending signal has a custom handler to run.
        if (signal_pending()) {
            uint64_t pending = current_task_ptr->pending_signals & ~current_task_ptr->blocked_signals;
            pending |= current_task_ptr->pending_signals & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)); // unblockable
            
            int has_custom = 0;
            for (int s = 1; s <= 31; s++) {
                if (!(pending & (1ULL << s))) continue;
                uint64_t h = current_task_ptr->sigactions[s * 4];
                if (h != 0 && h != 1) { has_custom = 1; break; }
                if (s != SIGCHLD && s != SIGCONT && s != SIGTSTP && s != SIGSTOP && h == 0) { has_custom = 1; break; }
            }
            if (has_custom) {
                current_task_ptr->waiting_for = 0;
                frame->rax = (uint64_t)-EINTR;
                spin_unlock(&sched_lock);
                return;
            }
        }

        current_task_ptr->waiting_for = pid > 0 ? pid : -1;
        current_task_ptr->state = TASK_STOPPED;
        spin_unlock(&sched_lock);
        yield_sched();
        spin_lock(&sched_lock);
        current_task_ptr->waiting_for = 0;
    }
}

void sys_kill(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;

    // Accept signal 0 (existence check) and standard POSIX signals 1-31
    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }

    if (pid > 0) {
        // Send to specific process
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
                if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) {
                    frame->rax = (uint64_t)-EPERM; return;
                }
                if (pid == 1 && sig != 0 && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
                if (sig == 0) { frame->rax = 0; return; }
                send_task_signal(i, sig);
                frame->rax = 0;
                return;
            }
        }
        frame->rax = (uint64_t)-ESRCH;
        return;
    }

    if (pid == 0) {
        // Send to every process in caller's process group
        pid_t my_pgrp = current_task_ptr->pgid;
        int found = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state == TASK_DEAD) continue;
            if (tasks[i]->pgid != my_pgrp) continue;
            if (sig == 0) { found = 1; continue; }
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) continue;
            send_task_signal(i, sig);
            found = 1;
        }
        frame->rax = found ? 0 : (uint64_t)-ESRCH;
        return;
    }

    if (pid == -1) {
        // Send to every process the caller may signal (all except PID 1)
        int found = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state == TASK_DEAD) continue;
            if (tasks[i]->pid == 1 || tasks[i]->pid == current_task_ptr->pid) continue;
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) continue;
            if (sig != 0) send_task_signal(i, sig);
            found = 1;
        }
        frame->rax = found ? 0 : (uint64_t)-ESRCH;
        return;
    }

    // pid < -1: send to process group -pid
    pid_t target_pgrp = -pid;
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->pgid != target_pgrp) continue;
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) continue;
        if (sig != 0) send_task_signal(i, sig);
        found = 1;
    }
    frame->rax = found ? 0 : (uint64_t)-ESRCH;
}

void sys_uname(syscall_frame_t *frame) {
    uint64_t bufp = frame->rdi;

    if (!bufp || !user_write_range_ok(current_task_ptr->ctx, bufp, sizeof(struct utsname))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    struct utsname info = utsname;
    get_hostname(info.nodename, sizeof(info.nodename));
    get_domainname(info.domainname, sizeof(info.domainname));

    if (write_vmm(current_task_ptr->ctx, bufp, &info, sizeof(info)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

static flock_obj_t *find_advisory_lock_conflict(const fd_entry_t *entry, int requested_type, bool process_lock) {
    for (int i = 0; i < 128; i++) {
        flock_obj_t *obj = &global_flocks[i];
        if (!obj->used || obj->lock_type == 0 || strcmp(obj->path, entry->path) != 0) continue;
        if (process_lock && obj->owner_pid == current_task_ptr->pid) continue;
        if (!process_lock && obj == (flock_obj_t *)entry->handle) continue;
        if (requested_type == LOCK_EX || obj->lock_type == LOCK_EX) return obj;
    }
    return NULL;
}

static int set_advisory_lock(fd_entry_t *entry, int lock_type, bool nonblocking, bool process_lock) {
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
        sleep(10);
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

void sys_fcntl(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int cmd = (int)frame->rsi;
    uint64_t arg = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    switch (cmd) {
        case F_DUPFD: {
            int start = (int)arg;
            if (start < 0 || start >= FD_MAX) { frame->rax = (uint64_t)-EINVAL; return; }
            fd_table_t *table = &current_task_ptr->fd_table;
            for (int i = start; i < FD_MAX; i++) {
                if (!table->entries[i].open) {
                    table->entries[i] = *entry;
                    table->entries[i].open = true;
                    retain_fd_entry(&table->entries[i]);
                    frame->rax = (uint64_t)i;
                    return;
                }
            }
            frame->rax = (uint64_t)-EMFILE;
            return;
        }
        case F_GETFD:
            frame->rax = 0;
            return;
        case F_SETFD:
            frame->rax = 0;
            return;
        case F_GETFL:
            frame->rax = (uint64_t)entry->flags;
            return;
        case F_SETFL:
            // Only allow setting safe flags
            entry->flags = (entry->flags & ~F_SETFL_MASK) | ((uint32_t)arg & F_SETFL_MASK);
            frame->rax = 0;
            return;
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW: {
            if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4) { frame->rax = (uint64_t)-EBADF; return; }
            struct flock lock;
            if (copy_from_user(&lock, (const void *)arg, sizeof(lock)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK && lock.l_type != F_UNLCK) { frame->rax = (uint64_t)-EINVAL; return; }
            if (lock.l_whence != SEEK_SET && lock.l_whence != SEEK_CUR && lock.l_whence != SEEK_END) { frame->rax = (uint64_t)-EINVAL; return; }

            int requested_type = lock.l_type == F_RDLCK ? LOCK_SH : (lock.l_type == F_WRLCK ? LOCK_EX : 0);
            if (cmd == F_GETLK) {
                flock_obj_t *conflict = requested_type ? find_advisory_lock_conflict(entry, requested_type, true) : NULL;
                if (conflict) {
                    lock.l_type = conflict->lock_type == LOCK_EX ? F_WRLCK : F_RDLCK;
                    lock.l_pid = conflict->owner_pid;
                } else {
                    lock.l_type = F_UNLCK;
                    lock.l_pid = 0;
                }
                if (copy_to_user((void *)arg, &lock, sizeof(lock)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                frame->rax = 0;
                return;
            }

            int status = set_advisory_lock(entry, requested_type, cmd == F_SETLK, true);
            frame->rax = (uint64_t)status;
            return;
        }
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_flock(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int operation = (int)frame->rsi;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4) { frame->rax = (uint64_t)-EBADF; return; }

    int op = operation & ~LOCK_NB;
    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN) { frame->rax = (uint64_t)-EINVAL; return; }
    int status = set_advisory_lock(entry, op == LOCK_UN ? 0 : op, (operation & LOCK_NB) != 0, false);
    frame->rax = (uint64_t)status;
}

// tmpfs and the initrd overlay are memory-resident and CPU-cache coherent.
// There is no backing device to flush, so successful validation is all these
// calls need. WBINVD here only stalls the machine and is not persistence.
void sys_fsync(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    frame->rax = 0;
}

void sys_fdatasync(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    frame->rax = 0;
}

// Helper: truncate a path (initrd or tmpfs) to 'length' bytes.
static int do_truncate_path(const char *abs_path, uint64_t length) {
    if (check_ext4_path(abs_path)) return -EROFS;
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

void sys_truncate(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    uint64_t length = frame->rsi;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }
    char path[256];
    int cr = copy_string_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    int r = do_truncate_path(abs_path, length);
    frame->rax = (uint64_t)r;
}

void sys_ftruncate(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint64_t length = frame->rsi;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (!fd_allows_write(entry)) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_EXT4) { frame->rax = (uint64_t)-EROFS; return; }

    // tmpfs: path-based, like initrd below.
    if (entry->type == FD_TMPFS) {
        struct stat st;
        if (!stat_tmpfs_to_kst(entry->path, &st, true) || !can_access_stat_mode(&st, 0, 1, 0)) {
            frame->rax = (uint64_t)-EACCES;
            return;
        }
        int r = truncate_tmpfs(entry->path, length);
        // Clamp the file offset if it is now past EOF.
        if (r == 0 && entry->offset > length) entry->offset = length;
        frame->rax = (uint64_t)r;
        return;
    }

    // initrd / overlay: use the path.
    if (entry->type == FD_FILE) {
        int r = do_truncate_path(entry->path, length);
        if (r == 0 && entry->offset > length) entry->offset = length;
        frame->rax = (uint64_t)r;
        return;
    }

    frame->rax = (uint64_t)-EINVAL;
}

void sys_getdents(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint64_t bufp = frame->rsi;
    uint64_t buflen = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (buflen == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint64_t written = 0;
    int index = (int)entry->offset;

    // Resolve symlinks in the directory path so that accessing a virtual FS
    // through a symlink (e.g. /dev-link -> /dev) still matches the correct mount.
    char resolved_path[256];
    resolve_dir_for_readdir(entry->path, resolved_path, sizeof(resolved_path), NULL, 0);

    if (entry->type == FD_EXT4 || check_ext4_path(resolved_path)) {
        struct stat st;
        int status = stat_ext4(resolved_path, &st, true);
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (!S_ISDIR(st.st_mode)) { frame->rax = (uint64_t)-ENOTDIR; return; }
        if (index == 0) {
            if (!emit_dirent(bufp, &written, buflen, st.st_ino, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1; entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent(bufp, &written, buflen, st.st_ino, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2; entry->offset = index;
        }
        int child_index = index - 2;
        while (1) {
            char child[256];
            uint8_t child_type;
            ino_t child_ino;
            status = get_next_ext4_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino);
            if (status != 0) break;
            if (!emit_dirent(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 2), child_type, child)) break;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // tmpfs directory enumeration (/tmp, /run, ...)  [getdents]
    // Path-based throughout, mirroring the plain initrd enumeration below
    // (next_tmpfs_child is the exact brother of next_initrd_child).
    if (entry->type == FD_TMPFS || is_tmpfs_dir(resolved_path)) {
        tmpfs_file_t dstat = stat_tmpfs(resolved_path);
        if (!dstat.mode || !S_ISDIR(dstat.mode)) {
            frame->rax = (uint64_t)-ENOTDIR; return;
        }
        if (index == 0) {
            if (!emit_dirent(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1; entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2; entry->offset = index;
        }
        int child_index = index - 2;
        while (1) {
            char child[256];
            uint8_t child_type = DT_REG;
            ino_t child_ino = 0;
            if (next_tmpfs_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino) != 0) break;
            if (!emit_dirent(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 1), child_type, child)) break;
            child_index++;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Check if this directory is a virtual device filesystem
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs)
    char rel[256];
    if (match_vfs_path(resolved_path, "devpts", rel)) {
        if (index == 0) {
            if (!emit_dirent(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1;
            entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2;
            entry->offset = index;
        }
        // Count total entries (ptmx + allocated slaves)
        int total_devs = 0;
        while (devpts_get_device_name(total_devs)) total_devs++;
        int dev_idx = index - 2;
        while (dev_idx < total_devs) {
            const char *devname = devpts_get_device_name(dev_idx);
            if (!devname) break;
            if (!emit_dirent(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_CHR, devname)) break;
            dev_idx++;
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    if (match_vfs_path(resolved_path, "devtmpfs", rel)) {
        // Emit . and .. for virtual filesystems too
        if (index == 0) {
            if (!emit_dirent(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1;
            entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2;
            entry->offset = index;
        }
        // Count total devices so sub-mount indexing is stable across calls
        int total_devs = 0;
        while (get_devtmpfs_device_name(total_devs)) total_devs++;
        // Emit registered devices
        int dev_idx = index - 2;
        while (dev_idx < total_devs) {
            const char *devname = get_devtmpfs_device_name(dev_idx);
            if (!devname) break;
            if (!emit_dirent(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_CHR, devname)) break;
            dev_idx++;
            index++;
            entry->offset = index;
        }
        // Emit sub-mount directories (e.g. /dev/pts under /dev)
        int sub_idx = (index - 2) - total_devs;
        if (sub_idx < 0) sub_idx = 0;
        while (1) {
            char sub_name[64];
            if (!find_vfs_submount(resolved_path, sub_idx, sub_name, sizeof(sub_name))) break;
            if (!emit_dirent(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_DIR, sub_name)) break;
            sub_idx++;
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // procfs directory enumeration: /proc, /proc/<pid>, /proc/<pid>/fd
    if (entry->type == FD_PROC || is_procfs_path(resolved_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!resolve_procfs(resolved_path, self, &n) || !is_procfs_dir(&n)) {
            frame->rax = (uint64_t)-ENOTDIR;
            return;
        }
        if (index == 0) {
            if (!emit_dirent(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1;
            entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2;
            entry->offset = index;
        }
        int child_index = index - 2;
        while (1) {
            char child[64];
            uint8_t child_type = DT_REG;
            if (!get_procfs_dirent(&n, self, child_index, child, sizeof(child), &child_type)) break;
            if (!emit_dirent(bufp, &written, buflen, (uint64_t)(child_index + 1), (uint64_t)(child_index + 1), child_type, child)) break;
            child_index++;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Normal initrd enumeration (resolved_path already has symlink resolution)
    // . at index 0, .. at index 1, real children from index 2+
    if (index == 0) {
        if (!emit_dirent(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
        index = 1;
        entry->offset = index;
    }
    if (index == 1) {
        if (!emit_dirent(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
        index = 2;
        entry->offset = index;
    }

    int child_index = index - 2;
    while (1) {
        char child[256];
        uint8_t child_type = DT_REG;
        ino_t child_ino = 0;
        if (next_initrd_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino) != 0) break;
        if (!emit_dirent(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 1), child_type, child)) break;
        child_index++;
        index = child_index + 2;
        entry->offset = index;
    }

    frame->rax = written;
}

void sys_getcwd(syscall_frame_t *frame) {
    uint64_t bufp = frame->rdi;
    uint64_t buflen = frame->rsi;
    
    if (!bufp || buflen == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    size_t cwd_len = strlen(current_task_ptr->cwd) + 1;
    if (cwd_len > buflen) { frame->rax = (uint64_t)-ERANGE; return; }
    
    char cwd_copy[256];
    if (cwd_len > sizeof(cwd_copy)) cwd_len = sizeof(cwd_copy);
    memcpy(cwd_copy, current_task_ptr->cwd, cwd_len);
    if (write_vmm(current_task_ptr->ctx, bufp, cwd_copy, cwd_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    
    frame->rax = cwd_len;
}

static int change_working_directory(const char *abs_path) {
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

    initrd_file_t dir = read_initrd(resolved);
    if (!dir.data && !dir.mode) return -ENOENT;
    if (!S_ISDIR(dir.mode) && strcmp(resolved, "/") != 0) return -ENOTDIR;

    strncpy(current_task_ptr->cwd, resolved, 255);
    current_task_ptr->cwd[255] = '\0';
    return 0;
}

void sys_chdir(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));
    frame->rax = (uint64_t)change_working_directory(abs_path);
}

void sys_fchdir(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4 && entry->type != FD_PROC && entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTDIR; return; }

    int status = change_working_directory(entry->path);
    frame->rax = (uint64_t)(status == -ENOENT ? -ENOTDIR : status);
}

void sys_rename(syscall_frame_t *frame) {
    const char *oldpath = (const char *)frame->rdi;
    const char *newpath = (const char *)frame->rsi;
    
    char old[256], new[256];
    if (copy_string_from_user(old, oldpath, sizeof(old)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_string_from_user(new, newpath, sizeof(new)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_old[256], abs_new[256];
    build_abs_path(old, abs_old, sizeof(abs_old));
    build_abs_path(new, abs_new, sizeof(abs_new));

    int access = reject_procfs_mutation(abs_old);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = reject_procfs_mutation(abs_new);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_old, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_new, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }

    // tmpfs: rename within (or across) tmpfs mounts.
    if (is_tmpfs_dir(abs_old) || is_tmpfs_dir(abs_new)) {
        frame->rax = (uint64_t)rename_tmpfs(abs_old, abs_new);
        return;
    }

    initrd_file_t file = read_initrd(abs_old);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    void *data_copy = NULL;
    if (file.data && file.size > 0) {
        data_copy = malloc(file.size);
        if (!data_copy) { frame->rax = (uint64_t)-ENOMEM; return; }
        memcpy(data_copy, file.data, file.size);
    }

    int ret = write_initrd(abs_new, data_copy, file.size, file.mode, file.uid, file.gid);
    if (data_copy) free(data_copy);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }
    delete_initrd(abs_old);
    frame->rax = 0;
}

void sys_mkdir(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));

    int access = reject_procfs_mutation(abs_path);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_path, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }

    if (is_tmpfs_dir(abs_path)) {
        frame->rax = (uint64_t)mkdir_tmpfs(abs_path, apply_current_umask(mode), current_task_ptr->fsuid, current_task_ptr->fsgid);
        return;
    }

    // Check if path already exists (POSIX: mkdir must fail with EEXIST)
    initrd_file_t existing = read_initrd(abs_path);
    if (existing.data || existing.mode) { frame->rax = (uint64_t)-EEXIST; return; }

    frame->rax = (uint64_t)mkdir_initrd(abs_path, apply_current_umask(mode), current_task_ptr->fsuid, current_task_ptr->fsgid);
}

void sys_rmdir(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));

    int access = reject_virtual_removal(abs_path);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_path, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }

    if (is_tmpfs_dir(abs_path)) {
        frame->rax = (uint64_t)rmdir_tmpfs(abs_path);
        return;
    }

    initrd_file_t file = stat_initrd(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (!S_ISDIR(file.mode)) { frame->rax = (uint64_t)-ENOTDIR; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) {frame->rax = (uint64_t)-EPERM; return; }

    int ret = rmdir_initrd(abs_path);
    frame->rax = ret < 0 ? (uint64_t)ret : 0;
}

void sys_link(syscall_frame_t *frame) {
    const char *oldpath = (const char *)frame->rdi;
    const char *newpath = (const char *)frame->rsi;

    char old[256], new[256];
    if (copy_string_from_user(old, oldpath, sizeof(old)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_string_from_user(new, newpath, sizeof(new)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_old[256], abs_new[256];
    build_abs_path(old, abs_old, sizeof(abs_old));
    build_abs_path(new, abs_new, sizeof(abs_new));

    int access = reject_procfs_mutation(abs_old);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = reject_procfs_mutation(abs_new);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_new, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }

    if (is_tmpfs_dir(abs_old) || is_tmpfs_dir(abs_new)) {
        frame->rax = (uint64_t)link_tmpfs(abs_old, abs_new);
        return;
    }

    initrd_file_t file = read_initrd(abs_old);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    void *data_copy = NULL;
    if (file.data && file.size > 0) {
        data_copy = malloc(file.size);
        if (!data_copy) { frame->rax = (uint64_t)-ENOMEM; return; }
        memcpy(data_copy, file.data, file.size);
    }

    int ret = write_initrd(abs_new, data_copy, file.size, file.mode, file.uid, file.gid);
    if (data_copy) free(data_copy);
    frame->rax = (uint64_t)ret;
}

void sys_unlink(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    get_absolute_path(path_buf, abs_path, sizeof(abs_path));

    int access = reject_virtual_removal(abs_path);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_path, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }

    // tmpfs entries live in RAM, not the initrd overlay.
    if (is_tmpfs_dir(abs_path)) {
        frame->rax = (uint64_t)delete_tmpfs(abs_path);
        return;
    }

    // Don't follow the final component: unlink must remove the link itself, // not its target.
    initrd_file_t file = stat_initrd_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    int ret = delete_initrd(abs_path);
    if (ret < 0) { frame->rax = (uint64_t)-ENOENT; return; }

    frame->rax = 0;
}

void sys_symlink(syscall_frame_t *frame) {
    const char *target = (const char *)frame->rdi;
    const char *linkpath = (const char *)frame->rsi;

    if (!target || !linkpath) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[256], linkpath_buf[256];
    if (copy_string_from_user(target_buf, target, sizeof(target_buf)) < 0 || copy_string_from_user(linkpath_buf, linkpath, sizeof(linkpath_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_linkpath_buf[256];
    get_absolute_path(linkpath_buf, abs_linkpath_buf, sizeof(abs_linkpath_buf));

    int access = reject_procfs_mutation(abs_linkpath_buf);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    access = check_parent_access(abs_linkpath_buf, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }

    if (is_tmpfs_dir(abs_linkpath_buf)) {
        frame->rax = (uint64_t)symlink_tmpfs(target_buf, abs_linkpath_buf, current_task_ptr->fsuid, current_task_ptr->fsgid);
        return;
    }

    frame->rax = (uint64_t)symlink_initrd(target_buf, abs_linkpath_buf, current_task_ptr->fsuid, current_task_ptr->fsgid);
}

void sys_readlink(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    char *buf = (char *)frame->rsi;
    size_t bufsiz = (size_t)frame->rdx;

    if (!user_path || !buf || bufsiz == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, bufsiz)) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    // Resolve symlinks in the PATH PREFIX only (follow_final=false): we want
    // to read the link target of the final component itself, not follow it.
    // This turns "/proc/self/cwd/bin" into "/bin" before the initrd lookup.
    {
        char resolved[256];
        resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), false);
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    // procfs symlinks: /proc/<pid>/exe, /proc/<pid>/cwd, /proc/<pid>/fd/<n>
    if (is_procfs_path(abs_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!resolve_procfs_nofollow(abs_path, self, &n)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (n.type != PROC_NODE_SYMLINK) {
            frame->rax = (uint64_t)-EINVAL;  // not a symlink
            return;
        }
        char target[256];
        int tlen = read_procfs_link(&n, self, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)-EINVAL; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = (uint64_t)ulen;
        return;
    }

    // tmpfs symlinks live in RAM.
    if (is_tmpfs_dir(abs_path)) {
        char target[256];
        int tlen = read_tmpfs_link(abs_path, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)tlen; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = (uint64_t)ulen;
        return;
    }

    if (check_ext4_path(abs_path)) {
        char target[256];
        int tlen = read_ext4_link(abs_path, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)tlen; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = (uint64_t)ulen;
        return;
    }

    // Must inspect the symlink itself, not its target.
    initrd_file_t file = stat_initrd_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (!S_ISLNK(file.mode)) { frame->rax = (uint64_t)-EINVAL; return; }

    size_t len = strlen((const char *)file.data);
    if (len > bufsiz) len = bufsiz;

    if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, file.data, len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = (uint64_t)len;
}

void sys_chmod(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    get_absolute_path(path_buf, abs_path, sizeof(abs_path));

    if (check_ext4_path(abs_path)) { frame->rax = (uint64_t)-EROFS; return; }

    if (is_tmpfs_dir(abs_path)) {
        struct stat tst;
        if (!stat_tmpfs_to_kst(abs_path, &tst, false)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (current_task_ptr->euid != 0 && current_task_ptr->euid != tst.st_uid) { frame->rax = (uint64_t)-EPERM; return; }
        frame->rax = (uint64_t)chmod_tmpfs(abs_path, mode & 0777);
        return;
    }

    initrd_file_t file = stat_initrd(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }
    int ret = chmod_initrd(abs_path, mode & 0777);

    frame->rax = ret < 0 ? (uint64_t)ret : 0;
}

void sys_fchmod(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_EXT4) { frame->rax = (uint64_t)-EROFS; return; }

    // tmpfs file: stat the inode to check ownership, then chmod by path.
    if (entry->type == FD_TMPFS) {
        struct stat tst;
        if (!stat_tmpfs_to_kst(entry->path, &tst, false)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (current_task_ptr->euid != 0 && current_task_ptr->euid != tst.st_uid) { frame->rax = (uint64_t)-EPERM; return; }
        frame->rax = (uint64_t)chmod_tmpfs(entry->path, mode & 0777);
        return;
    }

    // Permission check: only the file owner or root may chmod
    initrd_file_t file = read_initrd(entry->path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) {
        frame->rax = (uint64_t)-EPERM; return;
    }

    frame->rax = (uint64_t)chmod_initrd(entry->path, mode & 0777);
}

void sys_chown(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    uid_t uid = (uid_t)frame->rsi;
    gid_t gid = (gid_t)frame->rdx;
    if (!user_path) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int status = copy_string_from_user(path, user_path, sizeof(path));
    if (status < 0) { frame->rax = (uint64_t)status; return; }
    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));
    frame->rax = (uint64_t)change_path_ownership(abs_path, uid, gid, true);
}

void sys_fchown(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uid_t uid = (uid_t)frame->rsi;
    gid_t gid = (gid_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)change_path_ownership(entry->path, uid, gid, true);
}

void sys_lchown(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    uid_t uid = (uid_t)frame->rsi;
    gid_t gid = (gid_t)frame->rdx;
    if (!user_path) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int status = copy_string_from_user(path, user_path, sizeof(path));
    if (status < 0) { frame->rax = (uint64_t)status; return; }
    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));
    frame->rax = (uint64_t)change_path_ownership(abs_path, uid, gid, false);
}

void sys_umask(syscall_frame_t *frame) {
    mode_t mask = (mode_t)frame->rdi;
    mode_t old_mask = current_task_ptr->umask;
    current_task_ptr->umask = mask & 0777;
    frame->rax = (uint64_t)old_mask;
}

void sys_time(syscall_frame_t *frame) {
    time_t *result = (time_t *)frame->rdi;
    time_t seconds = (time_t)(time_get_realtime_us() / 1000000ULL);

    if (result) {
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)result, sizeof(*result))) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)result, &seconds, sizeof(seconds)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    frame->rax = (uint64_t)seconds;
}

void sys_gettimeofday(syscall_frame_t *frame) {
    struct timeval *tv = (struct timeval *)frame->rdi;
    struct timezone *tz = (struct timezone *)frame->rsi;

    if (tv && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)tv, sizeof(*tv))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (tz && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)tz, sizeof(*tz))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    if (tv) {
        uint64_t usec = time_get_realtime_us();

        struct timeval ktv;
        ktv.tv_sec = (time_t)(usec / 1000000ULL);
        ktv.tv_usec = (suseconds_t)(usec % 1000000ULL);
        if (write_vmm(current_task_ptr->ctx, (uint64_t)tv, &ktv, sizeof(ktv)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    if (tz) { struct timezone ktz = {0}; if (write_vmm(current_task_ptr->ctx, (uint64_t)tz, &ktz, sizeof(ktz)) < 0) { frame->rax = (uint64_t)-EFAULT; return; } }

    frame->rax = 0;
}

void sys_getrlimit(syscall_frame_t *frame) {
    int resource = (int)frame->rdi;
    rlimit_t *rlim = (rlimit_t *)frame->rsi;

    if (!rlim || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)rlim, sizeof(*rlim))) { frame->rax = (uint64_t)-EFAULT; return; }

    rlimit_t lim;
    int ret = fill_rlimit(resource, &lim);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)rlim, &lim, sizeof(lim)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_getrusage(syscall_frame_t *frame) {
    int who = (int)frame->rdi;
    struct rusage *usage = (struct rusage *)frame->rsi;

    if (!usage || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)usage, sizeof(*usage))) { frame->rax = (uint64_t)-EFAULT; return; }
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) { frame->rax = (uint64_t)-EINVAL; return; }

    struct rusage ru = {0};
    // TODO: populate ru_utime/ru_stime with real per-task CPU accounting.
    // Previously this incorrectly used get_monotonic_time_us() (system uptime)
    // as ru_stime, causing `time` to report sys ≈ real for any sleeping process.
    (void)who;

    if (write_vmm(current_task_ptr->ctx, (uint64_t)usage, &ru, sizeof(ru)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_sysinfo(syscall_frame_t *frame) {
    struct sysinfo *user_info = (struct sysinfo *)frame->rdi;
    if (!user_info || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_info, sizeof(*user_info))) { frame->rax = (uint64_t)-EFAULT; return; }

    struct sysinfo info = {0};
    info.uptime = (long)(get_monotonic_time_us() / 1000000ULL);
    get_load_averages(info.loads);
    info.totalram = get_total_pmm_memory();
    info.freeram = get_free_pmm_memory();
    info.procs = get_process_count();
    info.mem_unit = 1;

    if (copy_to_user(user_info, &info, sizeof(info)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_times(syscall_frame_t *frame) {
    tms_t *buf = (tms_t *)frame->rdi;
    // TODO: use real per-task CPU tick accounting for tms_utime/tms_stime.
    // Previously tms_stime was set to system uptime ticks, making sys time
    // appear equal to real elapsed time for any blocking/sleeping process.
    uint64_t elapsed_ticks = get_monotonic_time_us() / 10000ULL;

    if (buf) {
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, sizeof(*buf))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        tms_t t = {0};
        // tms_utime and tms_stime remain 0 until per-task CPU accounting is added
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, &t, sizeof(t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    frame->rax = (uint64_t)elapsed_ticks;
}

void sys_syslog(syscall_frame_t *frame) {
    int type = (int)frame->rdi;
    char *user_buf = (char *)frame->rsi;
    int size = (int)frame->rdx;

    if (type < SYSLOG_ACTION_CLOSE || type > SYSLOG_ACTION_SIZE_BUFFER) { frame->rax = (uint64_t)-EINVAL; return; }
    if (type != SYSLOG_ACTION_SIZE_BUFFER && (!current_task_ptr || current_task_ptr->euid != 0)) { frame->rax = (uint64_t)-EPERM; return; }

    switch (type) {
        case SYSLOG_ACTION_CLOSE:
        case SYSLOG_ACTION_OPEN:
            frame->rax = 0;
            return;
        case SYSLOG_ACTION_READ:
        case SYSLOG_ACTION_READ_ALL:
        case SYSLOG_ACTION_READ_CLEAR: {
            if (!user_buf || size < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            if (size && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_buf, (uint64_t)size)) { frame->rax = (uint64_t)-EFAULT; return; }
            size_t capacity = (size_t)size;
            if (capacity > get_log_capacity()) capacity = get_log_capacity();
            if (!capacity) { frame->rax = 0; return; }
            char *buffer = malloc(capacity);
            if (!buffer) { frame->rax = (uint64_t)-ENOMEM; return; }
            size_t length;
            if (type == SYSLOG_ACTION_READ) {
                while (!(length = read_stream_log(buffer, capacity))) {
                    if (signal_pending()) { free(buffer); frame->rax = (uint64_t)-EINTR; return; }
                    let_current_task_sleep(1000);
                }
            } else {
                length = type == SYSLOG_ACTION_READ_CLEAR ? read_clear_log(buffer, capacity) : read_log(buffer, capacity);
            }
            int result = copy_to_user(user_buf, buffer, length);
            free(buffer);
            frame->rax = result < 0 ? (uint64_t)result : length;
            return;
        }
        case SYSLOG_ACTION_CLEAR:
            clear_log();
            frame->rax = 0;
            return;
        case SYSLOG_ACTION_SIZE_UNREAD:
            frame->rax = get_log_size();
            return;
        case SYSLOG_ACTION_SIZE_BUFFER:
            frame->rax = get_log_capacity();
            return;
        case SYSLOG_ACTION_CONSOLE_OFF:
        case SYSLOG_ACTION_CONSOLE_ON:
            control_log_console(type, 0);
            frame->rax = 0;
            return;
        case SYSLOG_ACTION_CONSOLE_LEVEL:
            if (size < 1 || size > 8) { frame->rax = (uint64_t)-EINVAL; return; }
            control_log_console(type, size);
            frame->rax = 0;
            return;
    }
}

void sys_getuid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->uid;
}

void sys_getgid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->gid;
}

void sys_setuid(syscall_frame_t *frame) {
    uid_t uid = (uid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->uid = uid;
        current_task_ptr->euid = uid;
        current_task_ptr->fsuid = uid;
        frame->rax = 0;
        return;
    }

    if (uid == current_task_ptr->uid || uid == current_task_ptr->euid) {
        current_task_ptr->euid = uid;
        current_task_ptr->fsuid = uid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setgid(syscall_frame_t *frame) {
    gid_t gid = (gid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->gid = gid;
        current_task_ptr->egid = gid;
        current_task_ptr->fsgid = gid;
        frame->rax = 0;
        return;
    }

    if (gid == current_task_ptr->gid || gid == current_task_ptr->egid) {
        current_task_ptr->egid = gid;
        current_task_ptr->fsgid = gid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_geteuid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->euid;
}

void sys_getegid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->egid;
}

void sys_setpgid(syscall_frame_t *frame) {
    pid_t pid  = (pid_t)frame->rdi;
    pid_t pgid = (pid_t)frame->rsi;

    // pid == 0 means caller
    if (pid == 0) pid = current_task_ptr->pid;
    // pgid == 0 means use target's pid
    if (pgid == 0) pgid = pid;
    if (pgid < 0) { frame->rax = (uint64_t)-EINVAL; return; }

    // Find the target task
    task_t *target = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
            target = tasks[i];
            break;
        }
    }
    if (!target) { frame->rax = (uint64_t)-ESRCH; return; }

    // Target must be the caller or a child of the caller
    if (target->pid != current_task_ptr->pid && target->ppid != current_task_ptr->pid) {
        frame->rax = (uint64_t)-ESRCH; return;
    }

    // Can't change pgrp of a session leader
    if (target->pid == target->sid) {
        frame->rax = (uint64_t)-EPERM; return;
    }

    // Must stay within the same session
    if (target->sid != current_task_ptr->sid) {
        frame->rax = (uint64_t)-EPERM; return;
    }

    target->pgid = pgid;
    frame->rax = 0;
}

void sys_getppid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->ppid;
}

void sys_getpgrp(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->pgid;
}

void sys_setsid(syscall_frame_t *frame) {
    if (current_task_ptr->pid == current_task_ptr->pgid) {
        frame->rax = (uint64_t)-EPERM;
        return;
    }
    // Create a new session: caller becomes session leader and pgrp leader
    current_task_ptr->sid  = current_task_ptr->pid;
    current_task_ptr->pgid = current_task_ptr->pid;
    // Disassociate from controlling terminal
    current_task_ptr->ctty_idx = -1;
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_seteuid(syscall_frame_t *frame) {
    uid_t euid = (uid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->euid = euid;
        current_task_ptr->fsuid = euid;
        frame->rax = 0;
        return;
    }

    if (euid == current_task_ptr->uid || euid == current_task_ptr->euid) {
        current_task_ptr->euid = euid;
        current_task_ptr->fsuid = euid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setegid(syscall_frame_t *frame) {
    gid_t egid = (gid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->egid = egid;
        current_task_ptr->fsgid = egid;
        frame->rax = 0;
        return;
    }

    if (egid == current_task_ptr->gid || egid == current_task_ptr->egid) {
        current_task_ptr->egid = egid;
        current_task_ptr->fsgid = egid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setresuid(syscall_frame_t *frame) {
    uid_t ruid = (uid_t)frame->rdi;
    uid_t euid = (uid_t)frame->rsi;
    uid_t suid = (uid_t)frame->rdx;
    uid_t no_change = (uid_t)-1;

    bool privileged = current_task_ptr && current_task_ptr->euid == 0;
    if (!privileged) {
        if (ruid != no_change && ruid != current_task_ptr->uid && ruid != current_task_ptr->euid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (euid != no_change && euid != current_task_ptr->uid && euid != current_task_ptr->euid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (suid != no_change && suid != current_task_ptr->uid && suid != current_task_ptr->euid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
    }

    if (ruid != no_change) current_task_ptr->uid = ruid;
    if (euid != no_change) {
        current_task_ptr->euid = euid;
        current_task_ptr->fsuid = euid;
    }
    frame->rax = 0;
}

void sys_getresuid(syscall_frame_t *frame) {
    uid_t *ruid = (uid_t *)frame->rdi;
    uid_t *euid = (uid_t *)frame->rsi;
    uid_t *suid = (uid_t *)frame->rdx;

    if (!ruid || !euid || !suid) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)ruid, sizeof(uid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)euid, sizeof(uid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)suid, sizeof(uid_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uid_t r = current_task_ptr->uid;
    uid_t e = current_task_ptr->euid;
    uid_t s = current_task_ptr->euid;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)ruid, &r, sizeof(r)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)euid, &e, sizeof(e)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)suid, &s, sizeof(s)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_setresgid(syscall_frame_t *frame) {
    gid_t rgid = (gid_t)frame->rdi;
    gid_t egid = (gid_t)frame->rsi;
    gid_t sgid = (gid_t)frame->rdx;
    gid_t no_change = (gid_t)-1;

    bool privileged = current_task_ptr && current_task_ptr->euid == 0;
    if (!privileged) {
        if (rgid != no_change && rgid != current_task_ptr->gid && rgid != current_task_ptr->egid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (egid != no_change && egid != current_task_ptr->gid && egid != current_task_ptr->egid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (sgid != no_change && sgid != current_task_ptr->gid && sgid != current_task_ptr->egid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
    }

    if (rgid != no_change) current_task_ptr->gid = rgid;
    if (egid != no_change) {
        current_task_ptr->egid = egid;
        current_task_ptr->fsgid = egid;
    }
    frame->rax = 0;
}

void sys_getresgid(syscall_frame_t *frame) {
    gid_t *rgid = (gid_t *)frame->rdi;
    gid_t *egid = (gid_t *)frame->rsi;
    gid_t *sgid = (gid_t *)frame->rdx;

    if (!rgid || !egid || !sgid) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)rgid, sizeof(gid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)egid, sizeof(gid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)sgid, sizeof(gid_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    gid_t r = current_task_ptr->gid;
    gid_t e = current_task_ptr->egid;
    gid_t s = current_task_ptr->egid;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)rgid, &r, sizeof(r)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)egid, &e, sizeof(e)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)sgid, &s, sizeof(s)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_setfsuid(syscall_frame_t *frame) {
    uid_t fsuid = (uid_t)frame->rdi;
    uid_t previous = current_task_ptr->fsuid;

    if (current_task_ptr->euid == 0 || fsuid == current_task_ptr->uid || fsuid == current_task_ptr->euid || fsuid == current_task_ptr->fsuid)
        current_task_ptr->fsuid = fsuid;

    frame->rax = previous;
}

void sys_setfsgid(syscall_frame_t *frame) {
    gid_t fsgid = (gid_t)frame->rdi;
    gid_t previous = current_task_ptr->fsgid;

    if (current_task_ptr->euid == 0 || fsgid == current_task_ptr->gid || fsgid == current_task_ptr->egid || fsgid == current_task_ptr->fsgid)
        current_task_ptr->fsgid = fsgid;

    frame->rax = previous;
}

void sys_getpgid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    if (pid == 0) {
        frame->rax = (uint64_t)current_task_ptr->pgid;
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
            frame->rax = (uint64_t)tasks[i]->pgid;
            return;
        }
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_getsid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    if (pid == 0) {
        frame->rax = (uint64_t)current_task_ptr->sid;
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
            // Only allowed to query tasks in same session
            if (tasks[i]->sid != current_task_ptr->sid) {
                frame->rax = (uint64_t)-EPERM;
            } else {
                frame->rax = (uint64_t)tasks[i]->sid;
            }
            return;
        }
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_rt_sigtimedwait(syscall_frame_t *frame) {
    uint64_t set_ptr   = frame->rdi;
    uint64_t info_ptr  = frame->rsi;
    struct timespec *timeout = (struct timespec *)frame->rdx;

    if (!set_ptr || !user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)set_ptr, sizeof(uint64_t))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    // sigset_t is 128 bytes, but only the low 64 bits matter for signals 1..63.
    uint64_t want = 0;
    if (read_vmm(current_task_ptr->ctx, &want, set_ptr, sizeof(uint64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    // SIGKILL/SIGSTOP can't be caught this way.
    want &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    if (want == 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    int64_t timeout_us = -1; // -1 = wait forever
    if (timeout) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout, sizeof(struct timespec))) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
        struct timespec ts;
        if (read_vmm(current_task_ptr->ctx, &ts, (uint64_t)timeout, sizeof(struct timespec)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
        if (ts.tv_sec > 9000000000LL) ts.tv_sec = 9000000000LL; // cap to avoid overflow
        timeout_us = (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
    }

    uint64_t start_us = get_monotonic_time_us();

    for (;;) {
        // pending_signals is 1-indexed: bit i <=> signal i. Scan signals in @want.
        for (int sig = 1; sig < 64; sig++) {
            uint64_t bit = (1ULL << sig);
            if (!(want & (1ULL << (sig - 1)))) continue;
            if (!(current_task_ptr->pending_signals & bit)) continue;

            // Consume the first pending signal in @set, regardless of whether
            // a handler is installed. By the time we get here the signal is
            // blocked, so check_signals() has left it pending for us.
            current_task_ptr->pending_signals &= ~bit;

            if (info_ptr && user_write_range_ok(current_task_ptr->ctx, (uint64_t)(void *)info_ptr, sizeof(siginfo_t))) {
                siginfo_t si;
                memset(&si, 0, sizeof(si));
                si.si_signo  = sig;
                si.si_code   = SI_USER;
                si.si_pid    = current_task_ptr->pid;
                si.si_uid    = current_task_ptr->uid;
                (void)write_vmm(current_task_ptr->ctx, info_ptr, &si, sizeof(siginfo_t));
            }

            frame->rax = (uint64_t)sig;
            return;
        }

        // Timeout expired?
        if (timeout_us >= 0) {
            uint64_t elapsed = get_monotonic_time_us() - start_us;
            if ((int64_t)elapsed >= timeout_us) {
                frame->rax = (uint64_t)-EAGAIN;
                return;
            }
        }

        // Interrupted by an *unrelated* pending signal that has a handler?
        if (signal_pending()) {
            uint64_t pending = current_task_ptr->pending_signals & ~(current_task_ptr->blocked_signals << 1);
            pending |= current_task_ptr->pending_signals & ((1ULL << SIGKILL) | (1ULL << SIGSTOP));

            int interrupted = 0;
            for (int s = 1; s <= 31; s++) {
                if (!(pending & (1ULL << s))) continue;
                uint64_t h = current_task_ptr->sigactions[s * 4];
                if (h != 0 && h != 1) { interrupted = 1; break; }
                if (s != SIGCHLD && s != SIGCONT && s != SIGTSTP && s != SIGSTOP && h == 0) {
                    interrupted = 1; break;
                }
            }
            if (interrupted) {
                frame->rax = (uint64_t)-EINTR;
                return;
            }
        }

        // Yield to the scheduler until something changes.
        let_current_task_sleep(1000);
    }
}

void sys_utime(syscall_frame_t *frame) {
    struct syscall_utimbuf { time_t actime; time_t modtime; };
    const char *user_path = (const char *)frame->rdi;
    const struct syscall_utimbuf *user_times = (const struct syscall_utimbuf *)frame->rsi;
    if (!user_path) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int status = copy_string_from_user(path, user_path, sizeof(path));
    if (status < 0) { frame->rax = (uint64_t)status; return; }
    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    struct timespec now = time_get_realtime_ts();
    struct timespec atime = now, mtime = now;
    if (user_times) {
        struct syscall_utimbuf times;
        if (copy_from_user(&times, user_times, sizeof(times)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        atime = (struct timespec){ .tv_sec = times.actime, .tv_nsec = 0 };
        mtime = (struct timespec){ .tv_sec = times.modtime, .tv_nsec = 0 };
    }
    frame->rax = (uint64_t)set_path_times(abs_path, atime, true, mtime, true, true);
}

void sys_prctl(syscall_frame_t *frame) {
    int option = (int)frame->rdi;
    unsigned long arg2 = (unsigned long)frame->rsi;
    unsigned long arg3 = (unsigned long)frame->rdx;
    unsigned long arg4 = (unsigned long)frame->r10;
    unsigned long arg5 = (unsigned long)frame->r8;

    (void)arg3; (void)arg4; (void)arg5;

    switch (option) {
        case PR_SET_NAME: {
            // arg2 = pointer to a 16-byte (incl. NUL) name buffer in userspace
            const char *user_name = (const char *)arg2;
            if (!user_name || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_name, 16)) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            char buf[16];
            copy_from_user(buf, user_name, 16);
            buf[15] = '\0';
            strncpy(current_task_ptr->name, buf, sizeof(current_task_ptr->name) - 1);
            current_task_ptr->name[sizeof(current_task_ptr->name) - 1] = '\0';
            frame->rax = 0;
            return;
        }
        case PR_GET_NAME: {
            char *user_name = (char *)arg2;
            if (!user_name || !user_range_ok(current_task_ptr->ctx, (uint64_t)user_name, 16)) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            char buf[16];
            memset(buf, 0, sizeof(buf));
            strncpy(buf, current_task_ptr->name, sizeof(buf) - 1);
            buf[15] = '\0';
            if (copy_to_user(user_name, buf, 16) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
        case PR_SET_PDEATHSIG:
        case PR_GET_PDEATHSIG:
        case PR_GET_DUMPABLE:
        case PR_SET_DUMPABLE:
        case PR_GET_KEEPCAPS:
        case PR_SET_KEEPCAPS:
        case PR_GET_TIMING:
        case PR_SET_TIMING:
        case PR_GET_ENDIAN:
        case PR_SET_ENDIAN:
        case PR_GET_TSC:
        case PR_SET_TSC:
        case PR_GET_SECUREBITS:
        case PR_SET_SECUREBITS:
        case PR_GET_TIMERSLACK:
        case PR_SET_TIMERSLACK:
        case PR_GET_UNALIGN:
        case PR_SET_UNALIGN:
        case PR_GET_FPEMU:
        case PR_SET_FPEMU:
        case PR_GET_FPEXC:
        case PR_SET_FPEXC:
        case PR_GET_SECCOMP:
        case PR_SET_SECCOMP:
        case PR_CAPBSET_READ:
        case PR_CAPBSET_DROP:
            // Accepted but no-op: these features aren't implemented, so
            // return success to avoid breaking userspace that probes them.
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_arch_prctl(syscall_frame_t *frame) {
    int code = (int)frame->rdi;
    unsigned long addr = (unsigned long)frame->rsi;

    switch (code) {
        case ARCH_SET_FS:
            // Validate user-space address for FS base (TLS pointer)
            if (addr != 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, 1)) {
                frame->rax = (uint64_t)-EPERM; return;
            }
            current_task_ptr->fs_base = addr;
            write_msr(MSR_FS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_FS:
            if (!user_write_range_ok(current_task_ptr->ctx, addr, sizeof(uint64_t))) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            if (write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->fs_base, sizeof(uint64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        case ARCH_SET_GS:
            if (addr != 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, 1)) {
                frame->rax = (uint64_t)-EPERM; return;
            }
            current_task_ptr->gs_base = addr;
            write_msr(MSR_KERNEL_GS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_GS:
            if (!user_write_range_ok(current_task_ptr->ctx, addr, sizeof(uint64_t))) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            if (write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->gs_base, sizeof(uint64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_setrlimit(syscall_frame_t *frame) {
    int resource = (int)frame->rdi;
    rlimit_t *rlim = (rlimit_t *)frame->rsi;
    rlimit_t current;

    if (!rlim) { frame->rax = (uint64_t)-EFAULT; return; }
    int ret = fill_rlimit(resource, &current);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }

    rlimit_t requested;
    if (copy_from_user(&requested, rlim, sizeof(requested)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (requested.rlim_cur != current.rlim_cur || requested.rlim_max != current.rlim_max) { frame->rax = (uint64_t)-EPERM; return; }

    frame->rax = 0;
}

void sys_settimeofday(syscall_frame_t *frame) {
    const struct timeval *tv = (const struct timeval *)frame->rdi;
    const struct timezone *tz = (const struct timezone *)frame->rsi;

    if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }

    if (tz) {
        struct timezone ktz;
        if (copy_from_user(&ktz, tz, sizeof(ktz)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ktz.tz_minuteswest < -15 * 60 || ktz.tz_minuteswest > 15 * 60) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    if (!tv) { frame->rax = 0; return; }

    struct timeval ktv;
    if (copy_from_user(&ktv, tv, sizeof(ktv)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    if (ktv.tv_sec < 0 || ktv.tv_usec < 0 || ktv.tv_usec >= 1000000 || ktv.tv_sec > (TIME_T_MAX / 1000000)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t desired_us = ((uint64_t)ktv.tv_sec * 1000000ULL) + (uint64_t)ktv.tv_usec;
    time_set_realtime_us(desired_us);

    frame->rax = 0;
}

void sys_mount(syscall_frame_t *frame) {
    const char *source = (const char *)frame->rdi;
    const char *target = (const char *)frame->rsi;
    const char *fs_type = (const char *)frame->rdx;
    unsigned long mountflags = (unsigned long)frame->r10;
    const void *data = (const void *)frame->r8;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    if (!target || !fs_type) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[VFS_PATH_MAX];
    char fs_type_buf[VFS_FS_TYPE_MAX];
    char source_buf[VFS_SOURCE_MAX];
    char data_buf[64];
    strlcpy(source_buf, "none", sizeof(source_buf));
    data_buf[0] = '\0';
    if (copy_string_from_user(target_buf, target, sizeof(target_buf)) < 0 || copy_string_from_user(fs_type_buf, fs_type, sizeof(fs_type_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (source && copy_string_from_user(source_buf, source, sizeof(source_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (data && copy_string_from_user(data_buf, data, sizeof(data_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    if (!*target_buf || !*fs_type_buf) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    initrd_file_t dir = read_initrd(target_buf);
    if (!S_ISDIR(dir.mode) && !dir.data) {
        frame->rax = (uint64_t)-ENOENT; return;
    }

    frame->rax = (uint64_t)mount_vfs(source_buf, target_buf, fs_type_buf, mountflags, data_buf);
}

void sys_umount2(syscall_frame_t *frame) {
    const char *target = (const char *)frame->rdi;
    int flags = (int)frame->rsi;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    if (!target) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[VFS_PATH_MAX];
    if (copy_string_from_user(target_buf, target, sizeof(target_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    frame->rax = (uint64_t)unmount_vfs(target_buf, flags);
}

void sys_reboot(syscall_frame_t *frame) {
    uint32_t magic1 = (uint32_t)frame->rdi;
    uint32_t magic2 = (uint32_t)frame->rsi;
    uint32_t op     = (uint32_t)frame->rdx;
    void *arg  = (void *)frame->r10;

    if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
    bool valid_magic2 = magic2 == LINUX_REBOOT_MAGIC2 || magic2 == LINUX_REBOOT_MAGIC2A || magic2 == LINUX_REBOOT_MAGIC2B || magic2 == LINUX_REBOOT_MAGIC2C;
    if (magic1 != LINUX_REBOOT_MAGIC1 || !valid_magic2) { frame->rax = (uint64_t)-EINVAL; return; }

    switch (op) {
        case LINUX_REBOOT_CMD_RESTART:
            log("reboot: rebooting system\n");
            reboot();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_POWER_OFF:
            poweroff();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_HALT:
            halt();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_RESTART2: {
            char command[256];
            size_t i = 0;
            for (; i < sizeof(command) - 1; i++) {
                if (copy_from_user(&command[i], (const uint8_t *)arg + i, 1) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                if (command[i] == '\0') break;
            }
            command[sizeof(command) - 1] = '\0';
            log("reboot: rebooting system with '%s'\n", command);
            reboot();
            __builtin_unreachable();
        }
        case LINUX_REBOOT_CMD_CAD_ON:
            set_keyboard_cad_reboot(true);
            frame->rax = 0;
            return;
        case LINUX_REBOOT_CMD_CAD_OFF:
            set_keyboard_cad_reboot(false);
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}


void sys_sethostname(syscall_frame_t *frame) {
    const char *name = (const char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    bool priv = current_task_ptr && current_task_ptr->euid == 0;

    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }
    if (!name || len == 0 || len >= MAX_HOSTNAME_LEN) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)name, len)) { frame->rax = (uint64_t)-EFAULT; return; }

    char name_buf[MAX_HOSTNAME_LEN];
    if (read_vmm(current_task_ptr->ctx, name_buf, (uint64_t)name, len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    name_buf[len] = '\0';

    frame->rax = set_hostname(name_buf, strnlen(name_buf, len));
}

void sys_setdomainname(syscall_frame_t *frame) {
    const char *name = (const char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    bool priv = current_task_ptr && current_task_ptr->euid == 0;

    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }
    if (!name || len == 0 || len >= MAX_HOSTNAME_LEN) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)name, len)) { frame->rax = (uint64_t)-EFAULT; return; }

    char name_buf[MAX_DOMAINNAME_LEN];
    if (read_vmm(current_task_ptr->ctx, name_buf, (uint64_t)name, len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    name_buf[len] = '\0';

    frame->rax = set_domainname(name_buf, strnlen(name_buf, len));
}

void sys_gettid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_readahead(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    off64_t offset = (off64_t)frame->rsi;
    size_t count = (size_t)frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry || !entry->open) {
        frame->rax = (uint64_t)-EBADF;
        return;
    }

    // POSIX says ESPIPE, but Linux readahead() returns EINVAL for non-regular files
    if (entry->type == FD_PIPE   || entry->type == FD_SOCKET || entry->type == FD_STREAM || entry->type == FD_PTY_MASTER) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    // Device, procfs, and epoll fds don't support readahead (also EINVAL)
    if (entry->type == FD_DEV   || entry->type == FD_PROC  || entry->type == FD_EPOLL || entry->type == FD_EPOLL_H) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    // Linux silently returns 0 for count == 0 or negative offsets on valid files
    if (count == 0 || offset < 0) {
        frame->rax = 0;
        return;
    }

    // Clamp count to MAX_IO_COUNT so we don't over-allocate
    if (count > MAX_IO_COUNT)
        count = MAX_IO_COUNT;

    if (entry->type == FD_TMPFS) {
        // tmpfs: data is memory-resident; touch the range to "warm" it
        // (matches page-cache readahead semantics as closely as possible
        // in a non-paged kernel), mirroring the FD_FILE/initrd path below.
        tmpfs_file_t file = read_tmpfs(entry->path);
        if (S_ISDIR(file.mode)) {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
        if (!file.data) {
            frame->rax = (uint64_t)-EBADF;
            return;
        }
        if ((uint64_t)offset < file.size) {
            uint64_t avail = file.size - (uint64_t)offset;
            volatile uint8_t *p = (volatile uint8_t *)file.data + (uint64_t)offset;
            uint64_t to_touch = avail < (uint64_t)count ? avail : (uint64_t)count;
            (void)*p;
            (void)*(p + to_touch - 1);
        }
        frame->rax = 0;
        return;
    }

    if (entry->type == FD_EXT4) {
        uint8_t *buffer = malloc(count);
        if (!buffer) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t status = read_ext4(entry->path, buffer, count, (uint64_t)offset);
        free(buffer);
        frame->rax = status < 0 ? (uint64_t)status : 0;
        return;
    }

    // FD_FILE (initrd): data is already fully in memory.
    // Validate the requested range lies within the file.
    initrd_file_t file = read_initrd(entry->path);
    // Directories return EINVAL for readahead
    if (S_ISDIR(file.mode)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if (!file.data) {
        // File no longer exists / unreadable
        frame->rax = (uint64_t)-EBADF;
        return;
    }
    // Offset past EOF is not an error for readahead — Linux silently succeeds.
    // We just touch whatever is available.
    if ((uint64_t)offset < file.size) {
        uint64_t avail = file.size - (uint64_t)offset;
        // "Touch" the bytes — the compiler won't optimise this away because
        // read_initrd() is an opaque call returning a live pointer.
        volatile uint8_t *p = (volatile uint8_t *)file.data + (uint64_t)offset;
        uint64_t to_touch = avail < (uint64_t)count ? avail : (uint64_t)count;
        // A single end-of-range touch is enough to demonstrate the range
        // is accessible; all data is already hot in RAM.
        (void)*p;
        (void)*(p + to_touch - 1);
    }

    frame->rax = 0;
}

void sys_tkill(syscall_frame_t *frame) {
    pid_t tid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;

    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->pid != tid) continue;
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (tid == 1 && sig != 0 && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
        if (sig == 0) { frame->rax = 0; return; }
        send_task_signal(i, sig);
        frame->rax = 0;
        return;
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_futex(syscall_frame_t *frame) {
    uint32_t *uaddr = (uint32_t *)frame->rdi;
    int op = (int)frame->rsi;
    uint32_t val = (uint32_t)frame->rdx;
    struct timespec *timeout_ptr = (struct timespec *)frame->r10;
    uint32_t *uaddr2 = (uint32_t *)frame->r8;
    uint32_t val3 = (uint32_t)frame->r9;

    int cmd = op & FUTEX_CMD_MASK;

    uint64_t phys = resolve_futex_key(uaddr, frame);
    if (!phys) return;

    switch (cmd) {

    case FUTEX_WAIT: {
        wait_futex(frame, phys, val, timeout_ptr, FUTEX_BITSET_MATCH_ANY, false);
        return;
    }

    case FUTEX_WAIT_BITSET: {
        if (val3 == 0) { frame->rax = (uint64_t)-EINVAL; return; }

        wait_futex(frame, phys, val, timeout_ptr, val3, true);
        return;
    }

    case FUTEX_WAKE: {
        if (val3 == 0) val3 = FUTEX_BITSET_MATCH_ANY;
        int woken = wake_futex(phys, val, FUTEX_BITSET_MATCH_ANY);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_WAKE_BITSET: {
        if (val3 == 0) { frame->rax = (uint64_t)-EINVAL; return; }
        int woken = wake_futex(phys, val, val3);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_REQUEUE: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_range_ok(current_task_ptr->ctx, (uint64_t)uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }

        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        int woken = 0, requeued = 0;
        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;

            if ((uint32_t)woken < val) {
                futex_waiters[i].state = FW_WOKEN;
                int idx = futex_waiters[i].task_idx;
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                    tasks[idx]->state = TASK_READY;
                woken++;
            } else if ((uint32_t)requeued < val2) {
                futex_waiters[i].phys_addr = phys2;
                requeued++;
            } else {
                break;
            }
        }

        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_CMP_REQUEUE: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_range_ok(current_task_ptr->ctx, (uint64_t)uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        uint32_t cur_val = 0;
        if (read_vmm(current_task_ptr->ctx, &cur_val, (uint64_t)uaddr, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }
        if (cur_val != val3) {
            spin_unlock_irqrestore(&futex_lock, irq_flags);
            frame->rax = (uint64_t)-EAGAIN;
            return;
        }

        int woken = 0, requeued = 0;
        for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;

            if ((uint32_t)woken < val) {
                futex_waiters[i].state = FW_WOKEN;
                int idx = futex_waiters[i].task_idx;
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                    tasks[idx]->state = TASK_READY;
                woken++;
            } else if ((uint32_t)requeued < val2) {
                futex_waiters[i].phys_addr = phys2;
                requeued++;
            } else {
                break;
            }
        }

        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_WAKE_OP: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        int      fop      = (int)((val3 >> 28) & 0x7U);
        int      fopshift = (int)((val3 >> 28) & 0x8U);
        uint32_t op_arg   = (val3 >> 12) & 0xFFFU;
        int      fcmp     = (int)((val3 >> 24) & 0xFU);
        uint32_t cmp_arg  = val3 & 0xFFFU;

        if (fopshift) op_arg = 1U << (op_arg & 0x1F);

        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        uint32_t oldval = 0;
        if (read_vmm(current_task_ptr->ctx, &oldval, (uint64_t)uaddr2, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }

        uint32_t newval = oldval;
        switch (fop) {
            case FUTEX_OP_SET:  newval = op_arg;           break;
            case FUTEX_OP_ADD:  newval = oldval + op_arg;  break;
            case FUTEX_OP_OR:   newval = oldval | op_arg;  break;
            case FUTEX_OP_ANDN: newval = oldval & ~op_arg; break;
            case FUTEX_OP_XOR:  newval = oldval ^ op_arg;  break;
            default:
                spin_unlock_irqrestore(&futex_lock, irq_flags);
                frame->rax = (uint64_t)-ENOSYS;
                return;
        }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)uaddr2, &newval, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }

        int woken = 0;
        for (int i = 0; i < MAX_FUTEX_WAITERS && (uint32_t)woken < val; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;
            futex_waiters[i].state = FW_WOKEN;
            int idx = futex_waiters[i].task_idx;
            if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                tasks[idx]->state = TASK_READY;
            woken++;
        }

        bool cond = false;
        switch (fcmp) {
            case FUTEX_OP_CMP_EQ: cond = (oldval == cmp_arg);                         break;
            case FUTEX_OP_CMP_NE: cond = (oldval != cmp_arg);                         break;
            case FUTEX_OP_CMP_LT: cond = ((int32_t)oldval <  (int32_t)cmp_arg);       break;
            case FUTEX_OP_CMP_LE: cond = ((int32_t)oldval <= (int32_t)cmp_arg);       break;
            case FUTEX_OP_CMP_GT: cond = ((int32_t)oldval >  (int32_t)cmp_arg);       break;
            case FUTEX_OP_CMP_GE: cond = ((int32_t)oldval >= (int32_t)cmp_arg);       break;
            default:
                spin_unlock_irqrestore(&futex_lock, irq_flags);
                frame->rax = (uint64_t)-ENOSYS;
                return;
        }

        if (cond) {
            for (int i = 0; i < MAX_FUTEX_WAITERS && (uint32_t)(woken - (int)val) < val2; i++) {
                if (futex_waiters[i].state != FW_WAITING) continue;
                if (futex_waiters[i].phys_addr != phys2) continue;
                futex_waiters[i].state = FW_WOKEN;
                int idx = futex_waiters[i].task_idx;
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                    tasks[idx]->state = TASK_READY;
                woken++;
            }
        }

        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)woken;
        return;
    }

    default:
        frame->rax = (uint64_t)-ENOSYS;
        return;
    }
}

void sys_sched_getaffinity(syscall_frame_t *frame) {
    pid_t pid    = (pid_t)frame->rdi;
    size_t size  = (size_t)frame->rsi;
    void *mask   = (void *)frame->rdx;

    (void)pid; // Currently unused

    int ncpus = cpu_count;
    if (ncpus <= 0) ncpus = 1;

    size_t needed = ((size_t)ncpus + 7) / 8;
    size_t needed_aligned = (needed + sizeof(unsigned long) - 1) & ~(sizeof(unsigned long) - 1);

    if (!mask) { frame->rax = (uint64_t)-EFAULT; return; }
    if (size < needed) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)mask, size)) { frame->rax = (uint64_t)-EFAULT; return; }

    unsigned long buf[1024];
    size_t bufsize = (needed_aligned > sizeof(buf)) ? sizeof(buf) : needed_aligned;
    memset(buf, 0, bufsize);

    for (int c = 0; c < ncpus; c++) {
        buf[c / (sizeof(unsigned long) * 8)] |= 1UL << (c % (sizeof(unsigned long) * 8));
    }

    if (copy_to_user(mask, buf, bufsize) != 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    frame->rax = (uint64_t)needed;
}

void sys_epoll_create(syscall_frame_t *frame) {
    int size = frame->rdi;
    (void)size; // size is ignored in modern Linux, but we still accept it for compatibility
    frame->rax = (uint64_t)(int64_t)do_epoll_create1(0);
}

void sys_getdents64(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint64_t bufp = frame->rsi;
    uint64_t buflen = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (buflen == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint64_t written = 0;
    int index = (int)entry->offset;

    // Resolve symlinks in the directory path so that accessing a virtual FS
    // through a symlink (e.g. /dev-link -> /dev) still matches the correct mount.
    char resolved_path[256];
    resolve_dir_for_readdir(entry->path, resolved_path, sizeof(resolved_path), NULL, 0);

    if (entry->type == FD_EXT4 || check_ext4_path(resolved_path)) {
        struct stat st;
        int status = stat_ext4(resolved_path, &st, true);
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (!S_ISDIR(st.st_mode)) { frame->rax = (uint64_t)-ENOTDIR; return; }
        if (index == 0) {
            if (!emit_dirent64(bufp, &written, buflen, st.st_ino, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1; entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent64(bufp, &written, buflen, st.st_ino, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2; entry->offset = index;
        }
        int child_index = index - 2;
        while (1) {
            char child[256];
            uint8_t child_type;
            ino_t child_ino;
            status = get_next_ext4_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino);
            if (status != 0) break;
            if (!emit_dirent64(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 2), child_type, child)) break;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // tmpfs directory enumeration (/tmp, /run, ...)
    // Path-based throughout (next_tmpfs_child is the exact brother of
    // next_initrd_child).
    if (entry->type == FD_TMPFS || is_tmpfs_dir(resolved_path)) {
        tmpfs_file_t dstat = stat_tmpfs(resolved_path);
        if (!dstat.mode || !S_ISDIR(dstat.mode)) {
            frame->rax = (uint64_t)-ENOTDIR; return;
        }
        if (index == 0) {
            if (!emit_dirent64(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1; entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent64(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2; entry->offset = index;
        }
        int child_index = index - 2;
        while (1) {
            char child[256];
            uint8_t child_type = DT_REG;
            ino_t child_ino = 0;
            if (next_tmpfs_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino) != 0) break;
            if (!emit_dirent64(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 1), child_type, child)) break;
            child_index++;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Check if this directory is a virtual device filesystem
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs)
    char rel[256];
    if (match_vfs_path(resolved_path, "devpts", rel)) {
        if (index == 0) {
            if (!emit_dirent64(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1;
            entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent64(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2;
            entry->offset = index;
        }
        // Count total entries (ptmx + allocated slaves)
        int total_devs = 0;
        while (devpts_get_device_name(total_devs)) total_devs++;
        int dev_idx = index - 2;
        while (dev_idx < total_devs) {
            const char *devname = devpts_get_device_name(dev_idx);
            if (!devname) break;
            if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_CHR, devname)) break;
            dev_idx++;
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    if (match_vfs_path(resolved_path, "devtmpfs", rel)) {
        if (index == 0) {
            if (!emit_dirent64(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1;
            entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent64(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2;
            entry->offset = index;
        }
        // Count total devices so sub-mount indexing is stable across calls
        int total_devs = 0;
        while (get_devtmpfs_device_name(total_devs)) total_devs++;
        // Emit registered devices
        int dev_idx = index - 2;
        while (dev_idx < total_devs) {
            const char *devname = get_devtmpfs_device_name(dev_idx);
            if (!devname) break;
            if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_CHR, devname)) break;
            dev_idx++;
            index++;
            entry->offset = index;
        }
        // Emit sub-mount directories (e.g. /dev/pts under /dev)
        int sub_idx = (index - 2) - total_devs;
        if (sub_idx < 0) sub_idx = 0;
        while (1) {
            char sub_name[64];
            if (!find_vfs_submount(resolved_path, sub_idx, sub_name, sizeof(sub_name))) break;
            if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_DIR, sub_name)) break;
            sub_idx++;
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // procfs directory enumeration: /proc, /proc/<pid>, /proc/<pid>/fd
    if (entry->type == FD_PROC || is_procfs_path(resolved_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!resolve_procfs(resolved_path, self, &n) || !is_procfs_dir(&n)) {
            frame->rax = (uint64_t)-ENOTDIR;
            return;
        }
        if (index == 0) {
            if (!emit_dirent64(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
            index = 1;
            entry->offset = index;
        }
        if (index == 1) {
            if (!emit_dirent64(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
            index = 2;
            entry->offset = index;
        }
        int child_index = index - 2;
        while (1) {
            char child[64];
            uint8_t child_type = DT_REG;
            if (!get_procfs_dirent(&n, self, child_index, child, sizeof(child), &child_type)) break;
            if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(child_index + 1), (uint64_t)(child_index + 1), child_type, child)) break;
            child_index++;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Normal initrd enumeration (resolved_path already has symlink resolution)
    // . at index 0, .. at index 1, real children from index 2+
    if (index == 0) {
        if (!emit_dirent64(bufp, &written, buflen, 1, 1, DT_DIR, ".")) { frame->rax = written; return; }
        index = 1;
        entry->offset = index;
    }
    if (index == 1) {
        if (!emit_dirent64(bufp, &written, buflen, 2, 2, DT_DIR, "..")) { frame->rax = written; return; }
        index = 2;
        entry->offset = index;
    }

    int child_index = index - 2;
    while (1) {
        char child[256];
        uint8_t child_type = DT_REG;
        ino_t child_ino = 0;
        if (next_initrd_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino) != 0) break;
        if (!emit_dirent64(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 1), child_type, child)) break;
        child_index++;
        index = child_index + 2;
        entry->offset = index;
    }

    frame->rax = written;
}

void sys_set_tid_address(syscall_frame_t *frame) {
    int *tidptr = (int *)frame->rdi;
    current_task_ptr->clear_child_tid = tidptr;
    frame->rax = current_task_ptr->pid;
}

void sys_clock_gettime(syscall_frame_t *frame) {
    int clk_id   = (int)frame->rdi;
    struct timespec *tp = (struct timespec *)frame->rsi;

    if (!tp || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)tp, sizeof(struct timespec))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    uint64_t us;
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        us = time_get_realtime_us();
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
        us = get_monotonic_time_us();
        break;
    default:
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    struct timespec ts = {
        .tv_sec  = (time_t)(us / 1000000ULL), .tv_nsec = (long)(us % 1000000ULL) * 1000L, };
    if (write_vmm(current_task_ptr->ctx, (uint64_t)tp, &ts, sizeof(ts)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_clock_getres(syscall_frame_t *frame) {
    int clk_id = (int)frame->rdi;
    struct timespec *resolution = (struct timespec *)frame->rsi;

    switch (clk_id) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_BOOTTIME:
            break;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }

    if (!resolution) {
        frame->rax = 0;
        return;
    }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)resolution, sizeof(*resolution))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    struct timespec result = { .tv_sec = 0, .tv_nsec = 1000 };
    if (write_vmm(current_task_ptr->ctx, (uint64_t)resolution, &result, sizeof(result)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_exit_group(syscall_frame_t *frame) {
    int status = (int)frame->rdi;
    vmm_context_t *group_ctx = current_task_ptr->ctx;

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *task = tasks[i];
        if (task->state == TASK_DEAD || task->ctx != group_ctx || task == current_task_ptr) continue;
        task->exit_status = status;
        task->pending_signals |= 1ULL << SIGKILL;
        if (task->state == TASK_STOPPED) task->state = TASK_READY;
    }

    exit_task(status);
}

void sys_epoll_wait(syscall_frame_t *frame) {
    int timeout_ms = (int)frame->r10;
    int64_t timeout_us = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms * 1000LL;
    do_epoll_wait(frame, timeout_us, 0);
}

void sys_epoll_ctl(syscall_frame_t *frame) {
    int epfd = (int)frame->rdi;
    int op   = (int)frame->rsi;
    int fd   = (int)frame->rdx;
    struct epoll_event *user_event = (struct epoll_event *)frame->r10;

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) { frame->rax = (uint64_t)-EBADF; return; }

    // The target fd must be valid
    fd_entry_t *target = get_current_fd(fd);
    if (!target || !target->open) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    // Cannot epoll an epoll fd (avoid loops)
    if (target->type == FD_EPOLL) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!user_event || !user_range_ok(current_task_ptr->ctx, (uint64_t)user_event, sizeof(struct epoll_event))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (epoll_find_interest(epi, fd) >= 0) {
            frame->rax = (uint64_t)-EEXIST; return;
        }
        if (epi->count >= MAX_EPOLL_INTERESTS) {
            frame->rax = (uint64_t)-ENOMEM; return;
        }
        struct epoll_event ev;
        if (copy_from_user(&ev, user_event, sizeof(ev)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        epoll_interest_t *interest = &epi->interests[epi->count];
        interest->watched_fd      = fd;
        interest->events          = ev.events;
        interest->data            = ev.data;
        interest->oneshot_reported = false;
        epi->count++;
        frame->rax = 0;
        break;
    }
    case EPOLL_CTL_MOD: {
        if (!user_event || !user_range_ok(current_task_ptr->ctx, (uint64_t)user_event, sizeof(struct epoll_event))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        int idx = epoll_find_interest(epi, fd);
        if (idx < 0) {
            frame->rax = (uint64_t)-ENOENT; return;
        }
        struct epoll_event ev;
        if (copy_from_user(&ev, user_event, sizeof(ev)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        epi->interests[idx].events           = ev.events;
        epi->interests[idx].data             = ev.data;
        epi->interests[idx].oneshot_reported = false;
        frame->rax = 0;
        break;
    }
    case EPOLL_CTL_DEL: {
        int idx = epoll_find_interest(epi, fd);
        if (idx < 0) {
            frame->rax = (uint64_t)-ENOENT; return;
        }
        // Swap with last and shrink
        epi->interests[idx] = epi->interests[epi->count - 1];
        epi->count--;
        frame->rax = 0;
        break;
    }
    default:
        frame->rax = (uint64_t)-EINVAL;
        break;
    }
}

void sys_tgkill(syscall_frame_t *frame) {
    pid_t tgid = (pid_t)frame->rdi;
    pid_t tid = (pid_t)frame->rsi;
    int sig = (int)frame->rdx;

    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }
    if (tid <= 0) { frame->rax = (uint64_t)-EINVAL; return; }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->pid != tid) continue;
        if (tgid > 0 && tasks[i]->pgid != current_task_ptr->pgid) {
            // tgkill requires the tid to be in the caller's thread group;
            // fallback: also allow same-pgid match for our process model.
        }
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (tid == 1 && sig != 0 && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
        if (sig == 0) { frame->rax = 0; return; }
        send_task_signal(i, sig);
        frame->rax = 0;
        return;
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_openat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    uint32_t flags = (uint32_t)frame->rdx;
    mode_t mode = (mode_t)frame->r10;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path_buf, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    // Resolve intermediate symlinks so /dev-clone/null -> /dev/null
    {
        char resolved[256];
        resolve_path_symlinks(abs_path, resolved, sizeof(resolved));
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    char rel_path[256];
    if (match_vfs_path(abs_path, "devtmpfs", rel_path)) {
        if (rel_path[0] != '\0' && !device_exists_on_devtmpfs(rel_path)) {
            initrd_file_t file = read_initrd(abs_path);
            if (!S_ISDIR(file.mode)) {
                frame->rax = (uint64_t)-ENOENT;
                return;
            }
        }
        if (rel_path[0] != '\0') {
            struct stat dev_st = {0};
            if (get_device_mode(rel_path, &dev_st.st_mode) == 0) {
                int want_write = (flags & O_ACCMODE) != O_RDONLY;
                int want_read = (flags & O_ACCMODE) != O_WRONLY;
                if (!can_access_stat_mode(&dev_st, want_read, want_write, 0)) {
                    frame->rax = (uint64_t)-EACCES;
                    return;
                }
            }
        }
        if (strcmp(rel_path, "ptmx") == 0 || strcmp(rel_path, "pts/ptmx") == 0) {
            int idx = alloc_pty();
            if (idx < 0) { frame->rax = (uint64_t)-ENOSPC; return; }
            char ptm_path[32];
            ptm_path[0]='p'; ptm_path[1]='t'; ptm_path[2]='m'; ptm_path[3]=':';
            if (idx < 10) { ptm_path[4]='0'+idx; ptm_path[5]='\0'; }
            else          { ptm_path[4]='1'; ptm_path[5]='0'+(idx-10); ptm_path[6]='\0'; }
            int fd = alloc_fd(&current_task_ptr->fd_table, ptm_path, FD_PTY_MASTER, flags);
            if (fd < 0) { release_pty_master(idx); frame->rax = (uint64_t)fd; return; }
            frame->rax = (uint64_t)fd;
            return;
        }
        int pty_idx = pty_slave_path_idx(rel_path);
        if (pty_idx >= 0) { int r = open_pty_slave(pty_idx); if (r < 0) { frame->rax = (uint64_t)r; return; } }
        int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_DEV, flags);
        if (fd < 0 && pty_idx >= 0)
            release_pty_slave(pty_idx);
        frame->rax = (uint64_t)fd;
        return;
    } else if (match_vfs_path(abs_path, "devpts", rel_path)) {
        if (strcmp(rel_path, "ptmx") == 0) {
            int idx = alloc_pty();
            if (idx < 0) { frame->rax = (uint64_t)-ENOSPC; return; }
            char ptm_path[32];
            ptm_path[0]='p'; ptm_path[1]='t'; ptm_path[2]='m'; ptm_path[3]=':';
            if (idx < 10) { ptm_path[4]='0'+idx; ptm_path[5]='\0'; }
            else          { ptm_path[4]='1'; ptm_path[5]='0'+(idx-10); ptm_path[6]='\0'; }
            int fd = alloc_fd(&current_task_ptr->fd_table, ptm_path, FD_PTY_MASTER, flags);
            if (fd < 0) { release_pty_master(idx); frame->rax = (uint64_t)fd; return; }
            frame->rax = (uint64_t)fd;
            return;
        }
        if (rel_path[0] != '\0' && !devpts_device_exists(rel_path)) {
            initrd_file_t file = read_initrd(abs_path);
            if (!S_ISDIR(file.mode)) {
                frame->rax = (uint64_t)-ENOENT;
                return;
            }
        }
        int pty_idx = 0;
        const char *p = rel_path;
        while (*p >= '0' && *p <= '9') { pty_idx = pty_idx * 10 + (*p - '0'); p++; }
        if (*p != '\0') pty_idx = -1;
        if (pty_idx >= 0 && pty_idx < NUM_PTYS) { int r = open_pty_slave(pty_idx); if (r < 0) { frame->rax = (uint64_t)r; return; } }
        int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_DEV, flags);
        if (fd < 0 && pty_idx >= 0 && pty_idx < NUM_PTYS)
            release_pty_slave(pty_idx);
        frame->rax = (uint64_t)fd;
        return;
    }

    // tmpfs: /tmp, /run (and any other tmpfs mount)
    {
        int tr = open_tmpfs_common(abs_path, flags, mode);
        if (tr != 1) { frame->rax = (uint64_t)tr; return; }
    }

    // procfs: /proc, /proc/self, /proc/<pid>, /proc/<pid>/{maps,mounts,exe,...}
    {
        int pr = proc_open_common(abs_path, sizeof(abs_path), flags);
        if (pr != 1) { frame->rax = (uint64_t)pr; return; }
    }

    {
        int er = open_ext4_common(abs_path, flags);
        if (er != 1) { frame->rax = (uint64_t)er; return; }
    }

    initrd_file_t file = read_initrd(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    int parent_access = check_parent_access(abs_path, false);
    if (parent_access < 0) { frame->rax = (uint64_t)parent_access; return; }

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int access = check_parent_access(abs_path, true);
        if (access < 0) { frame->rax = (uint64_t)access; return; }
        int r = write_initrd(abs_path, "", 0, apply_current_umask(mode) | S_IFREG, current_task_ptr->fsuid, current_task_ptr->fsgid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
        file = read_initrd(abs_path);
    }

    if (!can_access_initrd(&file, want_read, want_write, 0)) { frame->rax = (uint64_t)-EACCES; return; }

    // O_TRUNC: truncate existing regular file to zero length
    if ((flags & O_TRUNC) && !want_write) { frame->rax = (uint64_t)-EACCES; return; }
    if ((flags & O_TRUNC) && file.data && S_ISREG(file.mode)) {
        int r = write_initrd(abs_path, NULL, 0, file.mode, file.uid, file.gid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
    }

    int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_FILE, flags);
    frame->rax = (uint64_t)fd;
}

void sys_fchownat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    uid_t uid = (uid_t)frame->rdx;
    gid_t gid = (gid_t)frame->r10;
    int flags = (int)frame->r8;
    if (!user_path) { frame->rax = (uint64_t)-EFAULT; return; }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) { frame->rax = (uint64_t)-EINVAL; return; }

    char path[256];
    int status = copy_string_from_user(path, user_path, sizeof(path));
    if (status < 0) { frame->rax = (uint64_t)status; return; }
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH)) { frame->rax = (uint64_t)-ENOENT; return; }
        fd_entry_t *entry = get_current_fd(dirfd);
        if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
        if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4) { frame->rax = (uint64_t)-EINVAL; return; }
        frame->rax = (uint64_t)change_path_ownership(entry->path, uid, gid, true);
        return;
    }

    if (path[0] != '/' && dirfd != AT_FDCWD) {
        struct stat dir_stat;
        int status = stat_fd_to_kst(dirfd, &dir_stat);
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (!S_ISDIR(dir_stat.st_mode)) { frame->rax = (uint64_t)-ENOTDIR; return; }
    }

    char abs_path[256];
    status = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (status < 0) { frame->rax = (uint64_t)status; return; }
    frame->rax = (uint64_t)change_path_ownership(abs_path, uid, gid, !(flags & AT_SYMLINK_NOFOLLOW));
}

void sys_fstatat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    struct stat *st = (struct stat *)frame->rdx;
    int flags = (int)frame->r10;

    if (!user_path || !st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int cr = copy_string_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    int br = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (br < 0) { frame->rax = (uint64_t)br; return; }

    bool has_trailing_slash = false;
    size_t path_len = strlen(path);
    if (path_len > 0 && path[path_len - 1] == '/') {
        has_trailing_slash = true;
    }
    bool follow_final = ((flags & AT_SYMLINK_NOFOLLOW) == 0) || has_trailing_slash;
    {
        char resolved[256];
        resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), follow_final);
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (follow_final ? stat_tmpfs_to_kst(abs_path, &kst, true) : stat_tmpfs_to_kst(abs_path, &kst, false)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (stat_ext4_to_kst(abs_path, &kst, follow_final)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (stat_proc(abs_path, path, &kst, follow_final)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }

    if (!stat_initrd_to_kst(abs_path, &kst, follow_final)) { frame->rax = (uint64_t)-ENOENT; return; }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}


void sys_unlinkat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    int flags = (int)frame->rdx;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }
    if (flags & ~AT_REMOVEDIR) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path_buf, abs_path, sizeof(abs_path));

    if (res < 0) { frame->rax = (uint64_t)res; return; }

    int mutation_status = reject_virtual_removal(abs_path);
    if (mutation_status < 0) { frame->rax = (uint64_t)mutation_status; return; }
    mutation_status = check_parent_access(abs_path, true);
    if (mutation_status < 0) { frame->rax = (uint64_t)mutation_status; return; }

    // tmpfs: RAM-backed, handle remove/rmdir directly.
    if (is_tmpfs_dir(abs_path)) {
        if (flags & AT_REMOVEDIR) {
            frame->rax = (uint64_t)rmdir_tmpfs(abs_path);
        } else {
            frame->rax = (uint64_t)delete_tmpfs(abs_path);
        }
        return;
    }

    // Don't follow the final component: unlinkat must remove the link itself.
    initrd_file_t file = stat_initrd_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    if (flags & AT_REMOVEDIR) {
        int ret = rmdir_initrd(abs_path);
        frame->rax = ret < 0 ? (uint64_t)ret : 0;
    } else {
        if (S_ISDIR(file.mode)) { frame->rax = (uint64_t)-EISDIR; return; }
        int ret = delete_initrd(abs_path);
        frame->rax = ret < 0 ? (uint64_t)-ENOENT : 0;
    }
}

void sys_symlinkat(syscall_frame_t *frame) {
    const char *target = (const char *)frame->rdi;
    int newdirfd = (int)frame->rsi;
    const char *user_path = (const char *)frame->rdx;

    if (!target || !user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[256];
    char path_buf[256];
    if (copy_string_from_user(target_buf, target, sizeof(target_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(newdirfd, path_buf, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    int mutation_status = reject_procfs_mutation(abs_path);
    if (mutation_status < 0) { frame->rax = (uint64_t)mutation_status; return; }
    mutation_status = check_parent_access(abs_path, true);
    if (mutation_status < 0) { frame->rax = (uint64_t)mutation_status; return; }

    if (is_tmpfs_dir(abs_path)) {
        frame->rax = (uint64_t)symlink_tmpfs(target_buf, abs_path, current_task_ptr->fsuid, current_task_ptr->fsgid);
        return;
    }

    // Existence check on the link path itself, not a resolved target.
    initrd_file_t existing = stat_initrd_nofollow(abs_path);
    if (existing.mode) { frame->rax = (uint64_t)-EEXIST; return; }

    int ret = symlink_initrd(target_buf, abs_path, current_task_ptr->fsuid, current_task_ptr->fsgid);

    frame->rax = ret < 0 ? (uint64_t)ret : 0;
}

void sys_readlinkat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    char *buf = (char *)frame->rdx;
    size_t bufsiz = (size_t)frame->r10;

    if (!user_path || !buf || bufsiz == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, bufsiz)) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int br = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (br < 0) { frame->rax = (uint64_t)br; return; }

    // Resolve symlinks in the PATH PREFIX only (follow_final=false): we want
    // to read the link target of the final component itself, not follow it.
    // This turns "/proc/self/cwd/bin" into "/bin" before the initrd lookup.
    {
        char resolved[256];
        resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), false);
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    if (is_procfs_path(abs_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!resolve_procfs_nofollow(abs_path, self, &n)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (n.type != PROC_NODE_SYMLINK) { frame->rax = (uint64_t)-EINVAL; return; }

        char target[256];
        int tlen = read_procfs_link(&n, self, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)-EINVAL; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = (uint64_t)ulen;
        return;
    }

    if (is_tmpfs_dir(abs_path)) {
        char target[256];
        int tlen = read_tmpfs_link(abs_path, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)tlen; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = (uint64_t)ulen;
        return;
    }

    if (check_ext4_path(abs_path)) {
        char target[256];
        int tlen = read_ext4_link(abs_path, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)tlen; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = (uint64_t)ulen;
        return;
    }

    initrd_file_t file = stat_initrd_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (!S_ISLNK(file.mode)) { frame->rax = (uint64_t)-EINVAL; return; }

    size_t len = strlen((const char *)file.data);
    if (len > bufsiz) len = bufsiz;

    if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, file.data, len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = (uint64_t)len;
}

void sys_fchmodat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    mode_t mode = (mode_t)frame->rdx;
    int flags = (int)frame->r10;

    (void)flags;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path_buf, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    if (check_ext4_path(abs_path)) { frame->rax = (uint64_t)-EROFS; return; }

    if (is_tmpfs_dir(abs_path)) {
        struct stat tst;
        if (!stat_tmpfs_to_kst(abs_path, &tst, false)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (current_task_ptr->euid != 0 && current_task_ptr->euid != tst.st_uid) { frame->rax = (uint64_t)-EPERM; return; }
        frame->rax = (uint64_t)chmod_tmpfs(abs_path, mode & 0777);
        return;
    }

    // Permission check: only owner or root can chmod
    initrd_file_t file = read_initrd(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    frame->rax = (uint64_t)chmod_initrd(abs_path, mode & 0777);
}

void sys_pselect6(syscall_frame_t *frame) {
    int nfds = (int)frame->rdi;
    uint64_t *readfds   = (uint64_t *)frame->rsi;
    uint64_t *writefds  = (uint64_t *)frame->rdx;
    uint64_t *exceptfds = (uint64_t *)frame->r10;
    struct timespec *timeout_ptr = (struct timespec *)frame->r8;
    // r9 points to {sigset_t *ss, size_t ss_len} — two pointers packed
    uint64_t sigmask_arg = frame->r9;

    if (nfds < 0 || nfds > FD_SETSIZE) { frame->rax = (uint64_t)-EINVAL; return; }

    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timespec))) { frame->rax = (uint64_t)-EFAULT; return; }
    }
    if (nfds > 0) {
        int bytes = ((nfds + 63) / 64) * 8;
        if (readfds   && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)readfds,   bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (writefds  && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)writefds,  bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (exceptfds && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)exceptfds, bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    int64_t timeout_us = -1;
    if (timeout_ptr) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout_ptr, sizeof(ts)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (timespec_to_us(&ts, &timeout_us) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    // Atomically swap signal mask
    uint64_t old_blocked = current_task_ptr->blocked_signals;
    if (sigmask_arg) {
        // pselect6 arg is a struct {sigset_t *ss; size_t ss_len} passed by pointer
        uint64_t ss_ptr = 0;
        size_t ss_len = 0;
        if (!user_range_ok(current_task_ptr->ctx, sigmask_arg, 16)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (read_vmm(current_task_ptr->ctx, &ss_ptr, sigmask_arg, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (read_vmm(current_task_ptr->ctx, &ss_len, sigmask_arg + 8, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ss_len != 8) { frame->rax = (uint64_t)-EINVAL; return; }
        if (ss_ptr) {
            uint64_t new_mask = 0;
            if (!user_range_ok(current_task_ptr->ctx, ss_ptr, 8)) { frame->rax = (uint64_t)-EFAULT; return; }
            if (read_vmm(current_task_ptr->ctx, &new_mask, ss_ptr, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            current_task_ptr->blocked_signals = new_mask;
        }
    }

    int set_bytes = nfds > 0 ? ((nfds + 7) / 8) : 0;
    int qword_bytes = nfds > 0 ? ((nfds + 63) / 64) * 8 : 0;

    uint8_t *k_read = NULL, *k_write = NULL, *k_except = NULL;
    uint8_t *o_read = NULL, *o_write = NULL, *o_except = NULL;

    if (set_bytes > 0) {
        k_read   = malloc(qword_bytes);
        k_write  = malloc(qword_bytes);
        k_except = malloc(qword_bytes);
        o_read   = malloc(qword_bytes);
        o_write  = malloc(qword_bytes);
        o_except = malloc(qword_bytes);
        if (!k_read || !k_write || !k_except || !o_read || !o_write || !o_except) {
            free(k_read); free(k_write); free(k_except);
            free(o_read); free(o_write); free(o_except);
            current_task_ptr->blocked_signals = old_blocked;
            frame->rax = (uint64_t)-ENOMEM; return;
        }
        memset(k_read,   0, qword_bytes);
        memset(k_write,  0, qword_bytes);
        memset(k_except, 0, qword_bytes);
        memset(o_read,   0, qword_bytes);
        memset(o_write,  0, qword_bytes);
        memset(o_except, 0, qword_bytes);
        if (readfds)   copy_from_user(k_read,   readfds,   set_bytes);
        if (writefds)  copy_from_user(k_write,  writefds,  set_bytes);
        if (exceptfds) copy_from_user(k_except, exceptfds, set_bytes);
    }

    int64_t ret = do_select(nfds, k_read, k_write, k_except, o_read, o_write, o_except, qword_bytes, timeout_us);

    // Restore signal mask BEFORE check_signals sees any delivered signal
    current_task_ptr->blocked_signals = old_blocked;

    if (ret >= 0) {
        if (readfds   && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)readfds,   o_read,   set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && writefds  && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)writefds,  o_write,  set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && exceptfds && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)exceptfds, o_except, set_bytes) < 0) ret = -EFAULT;
    }

    free(k_read); free(k_write); free(k_except);
    free(o_read); free(o_write); free(o_except);

    frame->rax = (uint64_t)ret;
}

void sys_set_robust_list(syscall_frame_t *frame) {
    void *head = (void *)frame->rdi;
    size_t len = (size_t)frame->rsi;

    // Linux documents the only supported struct size as 24 bytes
    // (sizeof(struct robust_list_head) on x86-64).
    if (len != 24) { frame->rax = (uint64_t)-EINVAL; return; }
    if (head && !user_range_ok(current_task_ptr->ctx, (uint64_t)head, len)) { frame->rax = (uint64_t)-EINVAL; return; }

    current_task_ptr->robust_list_head = head;
    frame->rax = 0;
}

void sys_get_robust_list(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    void **head_ptr = (void **)frame->rsi;
    size_t *len_ptr = (size_t *)frame->r10;

    if (pid != 0 && current_task_ptr && pid != current_task_ptr->pid) { frame->rax = (uint64_t)-ESRCH; return; }
    if (!head_ptr || !len_ptr || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)head_ptr, sizeof(void *)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)len_ptr, sizeof(size_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    void *head = current_task_ptr->robust_list_head;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)head_ptr, &head, sizeof(void *)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    size_t len = head ? 24 : 0;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)len_ptr, &len, sizeof(size_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_utimensat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    const struct timespec *user_times = (const struct timespec *)frame->rdx;
    int flags = (int)frame->r10;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) { frame->rax = (uint64_t)-EINVAL; return; }

    struct timespec atime, mtime;
    bool set_atime, set_mtime;
    int status = get_utimens_times(user_times, &atime, &set_atime, &mtime, &set_mtime);
    if (status < 0) { frame->rax = (uint64_t)status; return; }

    char abs_path[256];
    if (!user_path) {
        fd_entry_t *entry = get_current_fd(dirfd);
        if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
        strncpy(abs_path, entry->path, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    } else {
        char path[256];
        status = copy_string_from_user(path, user_path, sizeof(path));
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (path[0] == '\0') {
            if (!(flags & AT_EMPTY_PATH)) { frame->rax = (uint64_t)-ENOENT; return; }
            fd_entry_t *entry = get_current_fd(dirfd);
            if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
            strncpy(abs_path, entry->path, sizeof(abs_path) - 1);
            abs_path[sizeof(abs_path) - 1] = '\0';
        } else {
            status = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
            if (status < 0) { frame->rax = (uint64_t)status; return; }
        }
    }

    frame->rax = (uint64_t)set_path_times(abs_path, atime, set_atime, mtime, set_mtime, !(flags & AT_SYMLINK_NOFOLLOW));
}

void sys_epoll_pwait(syscall_frame_t *frame) {
    int epfd = (int)frame->rdi;
    struct epoll_event *user_events = (struct epoll_event *)frame->rsi;
    int maxevents = (int)frame->rdx;
    int timeout_ms = (int)frame->r10;
    uint64_t sigmask_ptr = frame->r8;
    uint64_t sigsetsize = frame->r9;

    int64_t timeout_us = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms * 1000LL;

    if (maxevents <= 0) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    if (maxevents > MAX_EPOLL_INTERESTS) maxevents = MAX_EPOLL_INTERESTS;

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) {
        frame->rax = (uint64_t)-EBADF; return;
    }

    size_t events_bytes = (size_t)maxevents * sizeof(struct epoll_event);
    if (!user_events || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_events, events_bytes)) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uint64_t old_blocked = current_task_ptr->blocked_signals;
    int mask_swapped = 0;

    if (sigmask_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)sigmask_ptr, sigsetsize)) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (sigsetsize != 8) {
            frame->rax = (uint64_t)-EINVAL; return;
        }        uint64_t new_mask = 0;
        if (read_vmm(current_task_ptr->ctx, &new_mask, sigmask_ptr, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        current_task_ptr->blocked_signals = new_mask;
        mask_swapped = 1;
    }

    int j = 0;
    for (int i = 0; i < epi->count; i++) {
        fd_entry_t *e = get_current_fd(epi->interests[i].watched_fd);
        if (e && e->open) {
            if (j != i) epi->interests[j] = epi->interests[i];
            j++;
        }
    }


    epi->count = j;

    struct epoll_event *k_events = malloc(events_bytes);
    if (!k_events) {
        if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;
        frame->rax = (uint64_t)-ENOMEM; return;
    }

    int count = epoll_collect(epi, k_events, maxevents);
    uint64_t start = get_monotonic_time_us();

    while (count == 0 && timeout_us != 0) {
        if (signal_pending()) {
            count = -EINTR;
            break;
        }
        if (timeout_us > 0 && (int64_t)(get_monotonic_time_us() - start) >= timeout_us) {
            count = 0;
            break;
        }

        spin_lock(&sched_lock);
        let_current_task_sleep(1000);
        spin_unlock(&sched_lock);

        count = epoll_collect(epi, k_events, maxevents);
    }

    if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;

    if (count > 0 && copy_to_user(user_events, k_events, (size_t)count * sizeof(struct epoll_event)) < 0) count = -EFAULT;
    free(k_events);
    frame->rax = (uint64_t)count;
}

void sys_epoll_create1(syscall_frame_t *frame) {
    int flags = (int)frame->rdi;
    frame->rax = (uint64_t)(int64_t)do_epoll_create1(flags);
}

void sys_pipe2(syscall_frame_t *frame) {
    int *pipefd = (int *)frame->rdi;
    int flags = (int)frame->rsi;
    unix_handle_t *read_end = NULL;
    unix_handle_t *write_end = NULL;
    int fds[2];
    int fd_flags;
    int r;

    if (!pipefd) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)pipefd, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) { frame->rax = (uint64_t)-EINVAL; return; }

    r = create_unix_pipe(&read_end, &write_end);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fd_flags = flags & O_NONBLOCK;
    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:r", FD_PIPE, O_RDONLY | fd_flags, read_end);
    if (fds[0] < 0) {
        release_unix_handle(read_end);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[0];
        return;
    }

    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:w", FD_PIPE, O_WRONLY | fd_flags, write_end);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)pipefd, fds, sizeof(fds)) < 0) { free_fd(&current_task_ptr->fd_table, fds[0]); free_fd(&current_task_ptr->fd_table, fds[1]); frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_prlimit64(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int resource = (int)frame->rsi;
    rlimit_t *new_rlim = (rlimit_t *)frame->rdx;
    rlimit_t *old_rlim = (rlimit_t *)frame->r10;

    // We don't yet maintain per-task limits, so only the caller itself is
    // queryable. (pid == current_task_ptr->pid is also fine, but pid == 0 is
    // the common "self" shorthand that glibc/bash uses.)
    if (pid != 0 && current_task_ptr && pid != current_task_ptr->pid) {
        frame->rax = (uint64_t)-EPERM;
        return;
    }

    rlimit_t current;
    int ret = fill_rlimit(resource, &current);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }

    if (old_rlim) {
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)old_rlim, sizeof(current))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)old_rlim, &current, sizeof(current)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    if (new_rlim) {
        rlimit_t requested;
        if (copy_from_user(&requested, new_rlim, sizeof(requested)) < 0) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (requested.rlim_cur != current.rlim_cur || requested.rlim_max != current.rlim_max) {
            frame->rax = (uint64_t)-EPERM;
            return;
        }
    }

    frame->rax = 0;
}

void sys_getrandom(syscall_frame_t *frame) {
    uint8_t *buf = (uint8_t *)frame->rdi;
    uint64_t buflen = frame->rsi;
    unsigned int flags = (unsigned int)frame->rdx;

    if (flags & ~(GRND_RANDOM | GRND_NONBLOCK | GRND_INSECURE)) { frame->rax = (uint64_t)-EINVAL; return; }
    if (buflen > 256) { frame->rax = (uint64_t)-EINVAL; return; }
    if (buflen == 0) { frame->rax = 0; return; }
    if (!buf) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    bool insecure = (flags & GRND_INSECURE);
    bool random = (flags & GRND_RANDOM);
    bool nonblock = (flags & GRND_NONBLOCK);

    if (random && !insecure && !is_rng_seeded()) {
        if (nonblock) {
            frame->rax = (uint64_t)-EAGAIN;
            return;
        }
    }

    uint64_t copied = 0;
    while (copied < buflen) {
        uint64_t rand_val;
        get_random_bytes(&rand_val, sizeof(rand_val));
        uint64_t to_copy = buflen - copied;
        if (to_copy > 8) to_copy = 8;
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf + copied, &rand_val, to_copy) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        copied += to_copy;
    }
    frame->rax = buflen;
}

void sys_statx(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    int flags = (int)frame->rdx;
    unsigned int mask = (unsigned int)frame->r10;
    struct statx *sx = (struct statx *)frame->r8;

    if (!sx) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)sx, sizeof(struct statx))) { frame->rax = (uint64_t)-EFAULT; return; }
    if (mask & STATX__RESERVED) { frame->rax = (uint64_t)-EINVAL; return; }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE)) { frame->rax = (uint64_t)-EINVAL; return; }
    if ((flags & AT_STATX_SYNC_TYPE) != AT_STATX_SYNC_AS_STAT && (flags & AT_STATX_SYNC_TYPE) != AT_STATX_FORCE_SYNC && (flags & AT_STATX_SYNC_TYPE) != AT_STATX_DONT_SYNC) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_path && !(flags & AT_EMPTY_PATH)) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    if (user_path) {
        int cr = copy_string_from_user(path, user_path, sizeof(path));
        if (cr < 0) { frame->rax = (uint64_t)cr; return; }
    } else {
        path[0] = '\0';
    }

    struct stat kst = {0};
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH)) { frame->rax = (uint64_t)-ENOENT; return; }
        int status;
        if (dirfd == AT_FDCWD) {
            char cwd[256];
            strncpy(cwd, current_task_ptr->cwd, sizeof(cwd) - 1);
            cwd[sizeof(cwd) - 1] = '\0';
            status = stat_tmpfs_to_kst(cwd, &kst, true) || stat_ext4_to_kst(cwd, &kst, true) || stat_proc(cwd, cwd, &kst, true) || stat_initrd_to_kst(cwd, &kst, true) || stat_virtual_device(cwd, &kst) ? 0 : -ENOENT;
        } else {
            status = stat_fd_to_kst(dirfd, &kst);
        }
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        struct statx ksx;
        const char *statx_path = dirfd == AT_FDCWD ? current_task_ptr->cwd : get_current_fd(dirfd)->path;
        stat_to_statx(&ksx, &kst, statx_path);
        statx_add_fs_metadata(&ksx, statx_path, true, mask);
        if (write_vmm(current_task_ptr->ctx, (uint64_t)sx, &ksx, sizeof(ksx)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }

    if (path[0] != '/' && dirfd != AT_FDCWD) {
        struct stat dir_stat;
        int status = stat_fd_to_kst(dirfd, &dir_stat);
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (!S_ISDIR(dir_stat.st_mode)) { frame->rax = (uint64_t)-ENOTDIR; return; }
    }

    char abs_path[256];
    int br = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (br < 0) { frame->rax = (uint64_t)br; return; }

    bool has_trailing_slash = false;
    size_t path_len = strlen(path);
    if (path_len > 0 && path[path_len - 1] == '/') has_trailing_slash = true;
    bool follow_final = ((flags & AT_SYMLINK_NOFOLLOW) == 0) || has_trailing_slash;
    {
        char resolved[256];
        resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), follow_final);
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    if (!((stat_virtual_device(abs_path, &kst)) || (follow_final ? stat_tmpfs_to_kst(abs_path, &kst, true) : stat_tmpfs_to_kst(abs_path, &kst, false)) || (stat_ext4_to_kst(abs_path, &kst, follow_final)) || (stat_proc(abs_path, path, &kst, follow_final)))) {
        if (!stat_initrd_to_kst(abs_path, &kst, follow_final)) { frame->rax = (uint64_t)-ENOENT; return; }
    }

    struct statx ksx;
    stat_to_statx(&ksx, &kst, abs_path);
    statx_add_fs_metadata(&ksx, abs_path, follow_final, mask);
    if (write_vmm(current_task_ptr->ctx, (uint64_t)sx, &ksx, sizeof(ksx)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_rseq(syscall_frame_t *frame) {
    struct rseq *rseq = (void *)frame->rdi;
    uint32_t rseq_len = (uint32_t)frame->rsi;
    int flags = (int)frame->rdx;
    uint32_t sig = (uint32_t)frame->r10;

    if (flags & ~RSEQ_FLAG_UNREGISTER) { frame->rax = (uint64_t)-EINVAL; return; }

    // NULL with unregister is a no-op success.
    if ((flags & RSEQ_FLAG_UNREGISTER) || !rseq) {
        current_task_ptr->rseq = NULL;
        current_task_ptr->rseq_len = 0;
        current_task_ptr->rseq_sig = 0;
        frame->rax = 0;
        return;
    }

    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)rseq, rseq_len)) { frame->rax = (uint64_t)-EFAULT; return; }

    current_task_ptr->rseq = rseq;
    current_task_ptr->rseq_len = rseq_len;
    current_task_ptr->rseq_sig = sig;
    frame->rax = 0;
}

void sys_epoll_pwait2(syscall_frame_t *frame) {
    int epfd = (int)frame->rdi;
    struct epoll_event *user_events = (struct epoll_event *)frame->rsi;
    int maxevents = (int)frame->rdx;
    struct timespec *timeout_ptr = (struct timespec *)frame->r10;
    uint64_t sigmask_ptr = frame->r8;
    uint64_t sigsetsize = frame->r9;

    int64_t timeout_us = -1; // infinite by default

    if (maxevents <= 0) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    if (maxevents > MAX_EPOLL_INTERESTS) maxevents = MAX_EPOLL_INTERESTS;

    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timespec))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        struct timespec ts;
        if (copy_from_user(&ts, timeout_ptr, sizeof(ts)) < 0) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (timespec_to_us(&ts, &timeout_us) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) {
        frame->rax = (uint64_t)-EBADF; return;
    }

    size_t events_bytes = (size_t)maxevents * sizeof(struct epoll_event);
    if (!user_events || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_events, events_bytes)) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uint64_t old_blocked = current_task_ptr->blocked_signals;
    int mask_swapped = 0;

    if (sigmask_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)sigmask_ptr, sigsetsize)) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (sigsetsize != 8) {
            frame->rax = (uint64_t)-EINVAL; return;
        }        uint64_t new_mask = 0;
        if (read_vmm(current_task_ptr->ctx, &new_mask, sigmask_ptr, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        current_task_ptr->blocked_signals = new_mask;
        mask_swapped = 1;
    }

    int j = 0;
    for (int i = 0; i < epi->count; i++) {
        fd_entry_t *e = get_current_fd(epi->interests[i].watched_fd);
        if (e && e->open) {
            if (j != i) epi->interests[j] = epi->interests[i];
            j++;
        }
    }


    epi->count = j;

    struct epoll_event *k_events = malloc(events_bytes);
    if (!k_events) {
        if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;
        frame->rax = (uint64_t)-ENOMEM; return;
    }

    int count = epoll_collect(epi, k_events, maxevents);
    uint64_t start = get_monotonic_time_us();

    while (count == 0 && timeout_us != 0) {
        if (signal_pending()) {
            count = -EINTR;
            break;
        }
        if (timeout_us > 0 && (int64_t)(get_monotonic_time_us() - start) >= timeout_us) {
            count = 0;
            break;
        }

        spin_lock(&sched_lock);
        let_current_task_sleep(1000);
        spin_unlock(&sched_lock);

        count = epoll_collect(epi, k_events, maxevents);
    }

    if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;

    if (count > 0 && copy_to_user(user_events, k_events, (size_t)count * sizeof(struct epoll_event)) < 0) count = -EFAULT;
    free(k_events);
    frame->rax = (uint64_t)count;
}
