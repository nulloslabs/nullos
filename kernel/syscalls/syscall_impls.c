#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/signal.h>
#include <freestanding/fcntl.h>
#include <freestanding/flock.h>
#include <freestanding/dirent.h>
#include <freestanding/time.h>
#include <freestanding/times.h>
#include <freestanding/wait.h>
#include <freestanding/termios.h>
#include <freestanding/limits.h>
#include <freestanding/poll.h>
#include <freestanding/errno.h>
#include <freestanding/asm/prctl.h>
#include <freestanding/sys/ioctl.h>
#include <freestanding/sys/socket.h>
#include <freestanding/sys/mman.h>
#include <freestanding/sys/fb.h>
#include <freestanding/sys/stat.h>
#include <freestanding/sys/resource.h>
#include <freestanding/sys/futex.h>
#include <freestanding/sys/reboot.h>
#include <freestanding/sys/random.h>
#include <freestanding/sys/uio.h>
#include <main/limine_req.h>
#include <io/devices.h>
#include <io/devtmpfs.h>
#include <io/devpts.h>
#include <io/pts_devices.h>
#include <main/spinlocks.h>
#include <main/elf.h>
#include <main/halt.h>
#include <main/hostname.h>
#include <main/timekeeping.h>
#include <main/utsname.h>
#include <main/acpi.h>
#include <main/msr.h>
#include <main/fd.h>
#include <main/rootfs.h>
#include <main/scheduler.h>
#include <main/string.h>
#include <main/rng.h>
#include <io/terminal.h>
#include <io/keyboard.h>
#include <io/ttys.h>
#include <io/ptys.h>
#include <io/hpet.h>
#include <io/sockets.h>
#include <io/serial.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/vma.h>
#include <io/procfs.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>

/* Tried to fucking modularize this...
   Didn't go well...
   To who is reading this:
     - Please don't try to modularize this, it's too late...just keep adding on...
*/

// Should be never exposed to other files
#define MAX_IOV 1024
#define MAX_MOUNTS 16
#define MAX_IO_COUNT (16 * 1024 * 1024)
#define MAX_FUTEX_WAITERS 256

#define FW_FREE       0
#define FW_WAITING    1
#define FW_WOKEN      2
#define FW_TIMED_OUT  3

typedef struct {
    int      state;
    uint64_t phys_addr;
    int      task_idx;
    uint32_t bitset;
    uint64_t deadline_us;
} futex_waiter_t;

typedef struct {
    char path[64];
    char filesystemtype[32];
    bool active;
} mount_t;


static mount_t mounts[MAX_MOUNTS];
static mode_t current_umask = 0022;
static futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];

static spinlock_t vfs_lock = SPINLOCK_INIT;
static spinlock_t stdin_lock = SPINLOCK_INIT;
static spinlock_t futex_lock = SPINLOCK_INIT;

static bool can_access_rootfs(const rootfs_file_t *file, int want_read, int want_write, int want_exec) {
    if (!file) return false;
    if (current_task_ptr && current_task_ptr->euid == 0) return true;

    int shift = 0;
    if (current_task_ptr->euid == file->uid) {
        shift = 6;
    } else if (current_task_ptr->egid == file->gid) {
        shift = 3;
    }

    int perm = (file->mode >> shift) & 7;
    if (want_read && !(perm & 4)) return false;
    if (want_write && !(perm & 2)) return false;
    if (want_exec && !(perm & 1)) return false;
    return true;
}

static void normalize_path_str(char *path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

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

    if (depth == 0) { strcpy(path, "/"); return; }

    char out[256];
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strcat(out, "/");
        strncat(out, parts[i], sizeof(out) - strlen(out) - 1);
    }
    strncpy(path, out, 255);
    path[255] = '\0';
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
        rootfs_file_t file = stat_rootfs_nofollow(work);
        work[end] = saved;

        if (file.mode && (file.mode & 0xF000) == 0xA000 && file.data) {
            link_count++;
            char link_abs[256];
            resolve_link_target(work, (const char *)file.data, link_abs, sizeof(link_abs));

            char rebuilt[256];
            strncpy(rebuilt, link_abs, sizeof(rebuilt) - 1);
            rebuilt[sizeof(rebuilt) - 1] = '\0';
            if (saved) {
                size_t cur_len = strlen(rebuilt);
                strncat(rebuilt, work + end, sizeof(rebuilt) - cur_len - 1);
            }
            normalize_path_str(rebuilt);
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
static void resolve_path_symlinks(const char *path, char *out, size_t out_size) {
    resolve_path_symlinks_ex(path, out, out_size, true);
}

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
    normalize_path_str(out);

    return 0;
}

static void build_abs_path(const char *path, char *out, size_t out_size) { build_abs_path_at(AT_FDCWD, path, out, out_size); }

static int copy_from_user_strarray(char ***out_karray, const char **user_arr, size_t max_elements) {
    char **k_arr = malloc((max_elements + 1) * sizeof(char *));
    if (!k_arr) return -ENOMEM;

    if (!user_arr) {
        k_arr[0] = NULL;
        *out_karray = k_arr;
        return 0;
    }

    if (!user_ptr_ok(user_arr, sizeof(char *))) {
        free(k_arr);
        return -EFAULT;
    }

    size_t count = 0;
    while (count < max_elements) {
        char *u_ptr = NULL;
        uint64_t user_element_addr = (uint64_t)&user_arr[count];

        if (!user_ptr_ok((void *)user_element_addr, sizeof(char *))) break;

        read_vmm(current_task_ptr->ctx, &u_ptr, user_element_addr, sizeof(char *));

        if (!u_ptr) break;

        char *k_str = malloc(256);
        if (!k_str) {
            for (size_t i = 0; i < count; i++) free(k_arr[i]);
            free(k_arr);
            return -ENOMEM;
        }

        read_vmm(current_task_ptr->ctx, k_str, (uint64_t)u_ptr, 255);
        k_str[255] = '\0';

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

// Find the Nth direct child mount under parent_path.
// E.g., if /dev/pts is mounted, get_sub_mount_name("/dev", 0, ...) returns "pts".
static const char *get_sub_mount_name(const char *parent_path, int n, char *name_buf, size_t buf_size) {
    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);

    size_t plen = strlen(parent_path);
    int count = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].active) continue;
        if (strncmp(mounts[i].path, parent_path, plen) != 0) continue;
        if (mounts[i].path[plen] != '/') continue;
        const char *rest = mounts[i].path + plen + 1;
        if (*rest == '\0' || strchr(rest, '/')) continue; // not a direct child
        if (count == n) {
            strncpy(name_buf, rest, buf_size - 1);
            name_buf[buf_size - 1] = '\0';
            spin_unlock_irqrestore(&vfs_lock, irq);
            return name_buf;
        }
        count++;
    }

    spin_unlock_irqrestore(&vfs_lock, irq);
    return NULL;
}

static bool stat_virtual_device(const char *abs_path, struct stat *kst) {
    // build_abs_path_at already resolved intermediate symlinks;
    // resolve the final component too for virtual device lookup.
    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));

    char rel_path[256];
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs),
    // so devtmpfs would incorrectly match /dev/pts paths with rel="pts".
    if (is_mounted_under(resolved, "devpts", rel_path)) {
        if (rel_path[0] == '\0') {
            kst->st_mode = 0040755;
            kst->st_nlink = 2;
        } else if (devpts_device_exists(rel_path)) {
            kst->st_mode = 0020620;
            kst->st_nlink = 1;
        } else {
            return false; // fall through to rootfs (symlinks, etc.)
        }
        kst->st_uid = 0; kst->st_gid = 0; kst->st_size = 0;
        kst->st_blocks = 0; kst->st_blksize = 4096;
        return true;
    }
    if (is_mounted_under(resolved, "devtmpfs", rel_path)) {
        if (rel_path[0] == '\0') {
            // The /dev directory itself
            kst->st_mode = 0040755;
            kst->st_nlink = 2;
        } else if (devtmpfs_device_exists(rel_path)) {
            kst->st_mode = 0020666; // character device, rw-rw-rw-
            kst->st_nlink = 1;
        } else {
            return false; // let rootfs handle it
        }
        kst->st_uid = 0; kst->st_gid = 0; kst->st_size = 0;
        kst->st_blocks = 0; kst->st_blksize = 4096;
        return true;
    }
    return false;
}

static int proc_self_idx(void) {
    if (!current_task_ptr) return -1;
    return current_task;  // current_task is the running task's index
}

static bool stat_proc(const char *abs_path, const char *orig_path, struct stat *kst, bool follow_self) {
    if (!procfs_is_proc_path(abs_path)) return false;
    int self = proc_self_idx();
    proc_node_t n;
    if (follow_self) {
        if (!procfs_resolve(abs_path, self, &n)) return false;
    } else {
        if (!procfs_resolve_nofollow_orig(abs_path, orig_path, self, &n)) return false;
    }

    kst->st_uid = current_task_ptr ? current_task_ptr->euid : 0;
    kst->st_gid = current_task_ptr ? current_task_ptr->egid : 0;
    kst->st_blocks = 0; kst->st_blksize = 4096; kst->st_nlink = 1;

    if (procfs_is_dir(&n)) {
        kst->st_mode = 0040555;  // dr-xr-xr-x
        kst->st_size = 0;
    } else if (n.type == PROC_NODE_SYMLINK) {
        kst->st_mode = 0120777;  // lrwxrwxrwx (symlink)
        kst->st_size = 0;
    } else {
        // Regular procfs file (maps, mounts): size is the content length.
        kst->st_mode = 0100444;  // -r--r--r--
        char tmp[PROCFS_MAX_CONTENT];
        kst->st_size = procfs_content(&n, tmp);
    }
    return true;
}

static int proc_open_common(const char *abs_path, uint32_t flags) {
    if (!procfs_is_proc_path(abs_path)) return 1;  // not procfs
    int self = proc_self_idx();
    proc_node_t n;
    if (!procfs_resolve(abs_path, self, &n)) return -ENOENT;

    // Directories open as regular directory fds (offset-tracked for getdents).
    if (procfs_is_dir(&n)) {
        return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_PROC, flags);
    }
    // Symlink nodes are openable but reads aren't meaningful;
    // we still allow open so readlink/stat work, matching Linux behavior.
    if (n.type == PROC_NODE_SYMLINK) {
        return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_PROC, flags);
    }
    // Regular procfs files (maps, mounts) are readable.
    if (n.entry == PROC_FILE_MAPS || n.entry == PROC_FILE_MOUNTS) {
        return alloc_fd(&current_task_ptr->fd_table, abs_path, FD_PROC, flags);
    }
    return -EACCES;
}

static void deliver_sig_to_task(int i, int sig) {
    if (sig == SIGKILL) {
        // SIGKILL cannot be ignored or caught
        if (tasks[i].pid == current_task_ptr->pid) {
            exit_task(128 + sig);
        }
        tasks[i].term_sig = sig;
        tasks[i].state = TASK_ZOMBIE;
        for (int j = 1; j < MAX_TASKS; j++) {
            if (tasks[j].state != TASK_DEAD && tasks[j].ppid == tasks[i].pid) {
                if (tasks[j].state == TASK_ZOMBIE) tasks[j].state = TASK_DEAD;
                else tasks[j].ppid = 1;
            }
        }
        for (int j = 0; j < FD_MAX; j++) {
            if (tasks[i].fd_table.entries[j].open) free_fd(&tasks[i].fd_table, j);
        }
        // Notify parent
        for (int j = 0; j < MAX_TASKS; j++) {
            if (tasks[j].state != TASK_DEAD && tasks[j].pid == tasks[i].ppid) {
                tasks[j].pending_signals |= (1ULL << SIGCHLD);
                if (tasks[j].state == TASK_STOPPED) tasks[j].state = TASK_READY;
                break;
            }
        }
        return;
    }
    switch (sig) {
        case SIGSTOP:
        case SIGTSTP:
            if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_READY) {
                tasks[i].state = TASK_STOPPED;
                tasks[i].stop_reported = 0;
                // Notify parent so waitpid(WUNTRACED) wakes up
                for (int j = 0; j < MAX_TASKS; j++) {
                    if (tasks[j].state != TASK_DEAD && tasks[j].pid == tasks[i].ppid) {
                        tasks[j].pending_signals |= (1ULL << SIGCHLD);
                        if (tasks[j].state == TASK_READY || tasks[j].state == TASK_RUNNING)
                            ; // already awake, will see the stopped child on next waitpid loop
                        break;
                    }
                }
                if (tasks[i].pid == current_task_ptr->pid) __asm__ volatile("int $32");
            }
            break;
        case SIGCONT:
            if (tasks[i].state == TASK_STOPPED) {
                tasks[i].state = TASK_READY;
                tasks[i].stop_reported = 0;
                // Notify parent of the continue event
                for (int j = 0; j < MAX_TASKS; j++) {
                    if (tasks[j].state != TASK_DEAD && tasks[j].pid == tasks[i].ppid) {
                        tasks[j].pending_signals |= (1ULL << SIGCHLD);
                        break;
                    }
                }
            }
            tasks[i].pending_signals &= ~((1ULL << SIGSTOP) | (1ULL << SIGTSTP));
            break;
        default:
            tasks[i].pending_signals |= (1ULL << sig);
            if (tasks[i].state == TASK_STOPPED)
                tasks[i].state = TASK_READY;
            break;
    }
}

static uint64_t futex_resolve_key(uint32_t *uaddr, syscall_frame_t *frame) {
    if (!uaddr || !user_ptr_ok(uaddr, sizeof(uint32_t))) {
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

static void futex_wait(syscall_frame_t *frame, uint64_t phys, uint32_t val, struct timespec *timeout_ptr, uint32_t bitset, bool absolute_timeout) {
    if (bitset == 0) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t deadline_us = 0;
    if (timeout_ptr) {
        if (!user_ptr_ok(timeout_ptr, sizeof(struct timespec))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        struct timespec ts;
        read_vmm(current_task_ptr->ctx, &ts, (uint64_t)timeout_ptr, sizeof(struct timespec));
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
    read_vmm(current_task_ptr->ctx, &cur_val,
             (uint64_t)(frame->rdi) , sizeof(uint32_t));
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

    spin_unlock_irqrestore(&futex_lock, irq_flags);

    while (1) {

        spin_unlock(&sched_lock);
        __asm__ volatile("int $32" ::: "memory");
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
        frame->rax = (uint64_t)-ETIMEDOUT;
    } else if (signal_pending()) {
        frame->rax = (uint64_t)-EINTR;
    } else {
        frame->rax = 0;
    }
}

static int futex_wake(uint64_t phys, uint32_t max_wake, uint32_t bitset) {
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
        if (idx >= 0 && idx < MAX_TASKS && tasks[idx].state == TASK_STOPPED)
            tasks[idx].state = TASK_READY;
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

    write_vmm(current_task_ptr->ctx, bufp + *written, rec, reclen);
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

    write_vmm(current_task_ptr->ctx, bufp + *written, rec, reclen);
    *written += reclen;
    return reclen;
}

static void resolve_dir_for_readdir(const char *fd_path, char *prefix_out, size_t prefix_size, char *abs_out, size_t abs_size) {
    // Follow symlinks by reading through read_rootfs and tracing the chain.
    char resolved[256];
    strncpy(resolved, fd_path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        rootfs_file_t file = stat_rootfs_nofollow(resolved);
        if (!file.mode) break; // doesn't exist at all
        if ((file.mode & 0xF000) != 0xA000) break; // not a symlink, done

        // Resolve symlink target relative to the symlink's parent directory
        const char *target = (const char *)file.data;
        if (!target) break;

        char link_abs[256];
        resolve_link_target(resolved, target, link_abs, sizeof(link_abs));
        strncpy(resolved, link_abs, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';

        // Normalize the resolved path
        normalize_path_str(resolved);
    }

    // Hand back the absolute path (next_rootfs_child builds its own prefix).
    if (abs_out) {
        strncpy(abs_out, resolved, abs_size - 1);
        abs_out[abs_size - 1] = '\0';
    }
    strncpy(prefix_out, resolved, prefix_size - 1);
    prefix_out[prefix_size - 1] = '\0';
}

static int next_rootfs_child(int *index, const char *dir_norm, char *child_name, size_t child_name_size, uint8_t *child_type) {
    char prefix[258];
    strncpy(prefix, dir_norm, sizeof(prefix) - 2);
    prefix[sizeof(prefix) - 2] = '\0';
    if (strcmp(dir_norm, "/") != 0) strcat(prefix, "/");
    size_t prefix_len = strlen(prefix);

    for (;;) {
        directory_entry_t de;
        if (get_rootfs_entry(*index, &de) != 0) return 1;

        const char *name = de.name;
        if (name[0] == '.' && name[1] == '/') name += 2;

        char name_clean[256];
        strncpy(name_clean, name, sizeof(name_clean) - 1);
        name_clean[sizeof(name_clean) - 1] = '\0';
        size_t nlen = strlen(name_clean);
        if (nlen > 1 && name_clean[nlen - 1] == '/') name_clean[--nlen] = '\0';

        char abs_entry[256];
        abs_entry[0] = '/';
        strncpy(abs_entry + 1, name_clean, sizeof(abs_entry) - 2);
        abs_entry[sizeof(abs_entry) - 1] = '\0';

        if (strncmp(abs_entry, prefix, prefix_len) != 0) { (*index)++; continue; }

        const char *child = abs_entry + prefix_len;
        if (!*child || strchr(child, '/')) { (*index)++; continue; }

        // "." and ".." are virtual entries we synthesize in getdents; a tar
        // entry for the directory itself (e.g. "./") collapses to "." here and
        // must not be emitted as a real child.
        if (strcmp(child, ".") == 0 || strcmp(child, "..") == 0) { (*index)++; continue; }

        strncpy(child_name, child, child_name_size - 1);
        child_name[child_name_size - 1] = '\0';
        *child_type = (de.type == FT_DIRECTORY) ? DT_DIR :
                      (de.type == FT_SYMLINK) ? DT_LNK : DT_REG;
        return 0;
    }
}

static int64_t read_dev_tty(char *kbuf, uint64_t count, int tty_idx) {
    tty_t *t = get_tty(tty_idx);
    if (!t) return (int64_t)-ENODEV;
    if (t->fg_pgrp > 0 && current_task_ptr->pgid != t->fg_pgrp) {
        tty_signal_pgrp(tty_idx, SIGTTIN);
        return (int64_t)-EINTR;
    }

    tcflag_t lflags = t->termios.c_lflag;
    cc_t vintr = t->termios.c_cc[VINTR];
    cc_t vsusp = t->termios.c_cc[VSUSP];
    uint64_t irq;

    if (!(lflags & ICANON)) {
        uint64_t total = 0;
        while (total < count) {
            // Try to grab whatever bytes are in the ring buffer right now.
            spinlock_t *lk = &tty_lock;
            spin_lock_irqsave(lk, &irq);
            int to_read = (int)(count - total);
            if (lflags & ISIG) to_read = 1; // Read 1 byte at a time so we can check for signals properly
            int got = read_tty_ring(&t->input, kbuf + total, to_read);
            spin_unlock_irqrestore(lk, irq);

            if (got > 0) {
                if (lflags & ISIG) {
                    char c = kbuf[total];
                    if (vintr && c == (char)vintr) {
                        uint64_t handler = current_task_ptr->sigactions[SIGINT * 4];
                        if (handler == 1) continue;
                        if (lflags & ECHO) { printf("^C"); putchar('\n'); }
                        if (total > 0) return (int64_t)total;
                        return (int64_t)-EINTR;
                    }
                    if (vsusp && c == (char)vsusp) {
                        uint64_t handler = current_task_ptr->sigactions[SIGTSTP * 4];
                        if (handler == 1) continue;
                        if (lflags & ECHO) { printf("^Z"); putchar('\n'); }
                        if (total > 0) return (int64_t)total;
                        return (int64_t)-EINTR;
                    }
                }
                total += (uint64_t)got;
                // In raw mode, a single read() should not block once it has
                // at least one byte — return what we have (VMIN=1, VTIME=0
                // semantics by default).
                break;
            }

            if (signal_pending()) return (int64_t)-EINTR;

            // Nothing available: re-enter the scheduler IRQ (which checks
            // sched_lock and skips if held).  Interrupts stay enabled (the
            // syscall handler did sti), so the keyboard ISR fires here,
            // fills the TTY ring, and the next loop iteration reads it.
            __asm__ volatile ("int $32");
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
            if (signal_pending()) return (int64_t)-EINTR;
            __asm__ volatile ("int $32");
        }

        spin_lock_irqsave(&stdin_lock, &irq);

        if (vintr && c == (char)vintr) {
            if (lflags & ISIG) {
                if (lflags & ECHO) { printf("^C"); putchar('\n'); }
                uint64_t handler = current_task_ptr->sigactions[SIGINT * 4];
                if (handler == 1 /* SIG_IGN */) {
                    // Shell is ignoring it: inject empty line so read() returns
                    *sbuf_len = 0; sbuf[0] = '\n'; *sbuf_len = 1;
                    spin_unlock_irqrestore(&stdin_lock, irq);
                    break;
                }
                *sbuf_len = 0;
                spin_unlock_irqrestore(&stdin_lock, irq);
                return (int64_t)-EINTR;
            }
        }
        if (vsusp && c == (char)vsusp) {
            if (lflags & ISIG) {
                uint64_t handler = current_task_ptr->sigactions[SIGTSTP * 4];
                if (handler == 1 /* SIG_IGN */) {
                    // Silently discard ^Z and keep reading (shell ignores SIGTSTP)
                    spin_unlock_irqrestore(&stdin_lock, irq);
                    continue;
                }
                if (lflags & ECHO) { printf("^Z"); putchar('\n'); }
                *sbuf_len = 0;
                spin_unlock_irqrestore(&stdin_lock, irq);
                return (int64_t)-EINTR;
            }
        }

        if (c == '\b' || c == 127) {
            if (*sbuf_len > 0) {
                (*sbuf_len)--;
                if (lflags & ECHO) printf("\b \b");
            }
            spin_unlock_irqrestore(&stdin_lock, irq);
            continue;
        }
        if (c == '\n') {
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

int copy_from_user(void *kdest, const void *usrc, size_t size) {
    if (!usrc || !user_ptr_ok(usrc, size)) return -EFAULT;
    if (!kdest || size == 0) return 0;
    read_vmm(current_task_ptr->ctx, kdest, (uint64_t)usrc, size);
    return 0;
}

int copy_to_user(const void *udest, const void *ksrc, size_t size) {
    if (!udest || !user_ptr_ok(udest, size)) return -EFAULT;
    if (!ksrc || size == 0) return 0;
    write_vmm(current_task_ptr->ctx, (uint64_t)udest, ksrc, size);
    return 0;
}

bool is_mounted_under(const char *path, const char *filesystemtype, char *relative_out) {
    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);
    bool found = false;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].active) continue;
        if (strcmp(mounts[i].filesystemtype, filesystemtype) != 0) continue;

        size_t mlen = strlen(mounts[i].path);
        if (strncmp(path, mounts[i].path, mlen) == 0 &&
            (path[mlen] == '/' || path[mlen] == '\0')) {
            if (relative_out) {
                const char *rel = path + mlen;
                if (*rel == '/') rel++;
                strcpy(relative_out, rel);
            }
            found = true;
            break;
        }
    }

    spin_unlock_irqrestore(&vfs_lock, irq);
    return found;
}

void register_vfs_mount(const char *path, const char *fstype) {
    if (!path || !fstype) return;
    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);
    // No-op if this exact (path, fstype) is already registered.
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].active &&
            strcmp(mounts[i].path, path) == 0 &&
            strcmp(mounts[i].filesystemtype, fstype) == 0) {
            spin_unlock_irqrestore(&vfs_lock, irq);
            return;
        }
    }
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            strncpy(mounts[i].path, path, sizeof(mounts[i].path) - 1);
            mounts[i].path[sizeof(mounts[i].path) - 1] = '\0';
            strncpy(mounts[i].filesystemtype, fstype, sizeof(mounts[i].filesystemtype) - 1);
            mounts[i].filesystemtype[sizeof(mounts[i].filesystemtype) - 1] = '\0';
            mounts[i].active = true;
            break;
        }
    }
    spin_unlock_irqrestore(&vfs_lock, irq);
}

int enumerate_vfs_mounts(int index, char *out_line, size_t line_size) {
    if (!out_line || line_size == 0) return 0;
    out_line[0] = '\0';

    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);

    int count = 0;
    int ret = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].active) continue;
        if (count != index) { count++; continue; }

        // Build "<dev> <mountpoint> <fstype> rw 0 0".
        // Device name is "none" for virtual filesystems; mountpoint and fstype
        // come from the mount entry.  Assemble manually (no snprintf freestanding).
        const char *dev = "none";
        const char *mp  = mounts[i].path;
        const char *ft  = mounts[i].filesystemtype;
        size_t w = 0;
        for (const char *s = dev; *s && w + 1 < line_size; ) out_line[w++] = *s++;
        if (w + 1 < line_size) out_line[w++] = ' ';
        for (const char *s = mp;  *s && w + 1 < line_size; ) out_line[w++] = *s++;
        if (w + 1 < line_size) out_line[w++] = ' ';
        for (const char *s = ft;  *s && w + 1 < line_size; ) out_line[w++] = *s++;
        if (w + 3 < line_size) { out_line[w++] = ' '; out_line[w++] = 'r'; out_line[w++] = 'w'; }
        if (w + 4 < line_size) { out_line[w++] = ' '; out_line[w++] = '0'; out_line[w++] = ' '; out_line[w++] = '0'; }
        out_line[w] = '\0';
        ret = (int)w;
        break;
    }

    spin_unlock_irqrestore(&vfs_lock, irq);
    return ret;
}

void check_signals(syscall_frame_t *frame) {
    if (!current_task_ptr) return;
    if (current_task_ptr->pending_signals == 0) return;

    for (int i = 1; i < 32; i++) {
        if (current_task_ptr->pending_signals & (1ULL << i)) {
            uint64_t *sa = &current_task_ptr->sigactions[i * 4];
            uint64_t handler = sa[0];
            uint64_t flags = sa[1];
            uint64_t restorer = sa[2];

            if (handler == 0 ) {
                // Default action depends on signal
                if (i == SIGTSTP || i == SIGSTOP) {
                    // Default action: stop the process
                    current_task_ptr->state = TASK_STOPPED;
                    current_task_ptr->stop_reported = 0;
                    current_task_ptr->pending_signals &= ~(1ULL << i);
                    // Notify parent so waitpid(WUNTRACED) wakes up
                    for (int _j = 0; _j < MAX_TASKS; _j++) {
                        if (tasks[_j].state != TASK_DEAD && tasks[_j].pid == current_task_ptr->ppid) {
                            tasks[_j].pending_signals |= (1ULL << SIGCHLD);
                            break;
                        }
                    }
                    // Yield to scheduler
                    __asm__ volatile("int $32");
                    continue;
                } else if (i == SIGCONT) {
                    // Default action: continue (already running, just clear)
                    current_task_ptr->pending_signals &= ~(1ULL << i);
                    continue;
                } else if (i == SIGCHLD) {
                    // Default action: ignore
                    current_task_ptr->pending_signals &= ~(1ULL << i);
                    continue;
                }
                // Default action: terminate
                current_task_ptr->pending_signals = 0;
                exit_task(128 + i);
            } else if (handler == 1 ) {
                current_task_ptr->pending_signals &= ~(1ULL << i);
                continue;
            }

            current_task_ptr->pending_signals &= ~(1ULL << i);

            uint64_t user_rsp = frame->rsp - 128; // red zone
            user_rsp -= sizeof(syscall_frame_t);
            user_rsp &= ~15ULL;

            write_vmm(current_task_ptr->ctx, user_rsp, frame, sizeof(syscall_frame_t));

            frame->rcx = handler;
            frame->rdi = i;

            if (flags & 4 ) {
                user_rsp -= 16; // siginfo_t is 12 bytes, align to 16
                uint32_t sinfo[4] = {i, 0, 0, 0};
                write_vmm(current_task_ptr->ctx, user_rsp, &sinfo, sizeof(sinfo));
                frame->rsi = user_rsp; // siginfo_t*
                frame->rdx = 0; // ucontext_t*
            }

            user_rsp -= 8;
            write_vmm(current_task_ptr->ctx, user_rsp, &restorer, sizeof(uint64_t));
            frame->rsp = user_rsp;

            break;
        }
    }
}

void futex_check_timeouts(void) {
    uint64_t now = time_get_realtime_us();

    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].state != FW_WAITING) continue;

        int idx = futex_waiters[i].task_idx;
        if (idx < 0 || idx >= MAX_TASKS) continue;

        if (futex_waiters[i].deadline_us != 0 &&
            now >= futex_waiters[i].deadline_us) {
            futex_waiters[i].state = FW_TIMED_OUT;
            if (tasks[idx].state == TASK_STOPPED)
                tasks[idx].state = TASK_READY;
            continue;
        }

        if (tasks[idx].pending_signals && tasks[idx].state == TASK_STOPPED) {
            futex_waiters[i].state = FW_WOKEN;
            tasks[idx].state = TASK_READY;
        }
    }
}

void wake_clear_child_tid(task_t *task) {
    if (task->clear_child_tid) {
        int zero = 0;
        write_vmm(task->ctx, (uint64_t)task->clear_child_tid, &zero, sizeof(int));
        uint64_t phys = get_vmm_phys(task->ctx, (uint64_t)task->clear_child_tid);
        if (phys) {
            futex_wake(phys, 1, 0xFFFFFFFFU);
        }
    }
}

void sys_read(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint8_t *buf = (uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    // Validate user buffer
    if (count > 0 && !user_ptr_ok(buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_STREAM) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        // For FD_STREAM, use the process's controlling terminal if set
        // This handles cases where a process has been assigned a TTY via TIOCSCTTY
        int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 0;
        // If ctty_idx not set, try to parse from path (e.g., /dev/tty1 -> 1, /dev/tty0 -> 0)
        if (tty_idx == 0) {
            const char *path = entry->path;
            if (strstr(path, "tty")) {
                const char *p = strstr(path, "tty");
                if (p && p[3] >= '0' && p[3] <= '7') {
                    tty_idx = p[3] - '0';
                }
            }
        }
        int64_t got = read_dev_tty((char *)kbuf, count, tty_idx);
        if (got >= 0) { write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got); }
        free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (is_mounted_under(entry->path, "devtmpfs", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }

            if (strncmp(rel, "tty", 3) == 0 || strcmp(rel, "console") == 0) {
                uint8_t *kbuf = malloc(count);
                if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
                // Parse TTY index from rel (e.g., "tty1" -> 1, "tty0" -> 0)
                int tty_idx = 0;
                if (strlen(rel) > 3 && rel[3] >= '0' && rel[3] <= '7') {
                    tty_idx = rel[3] - '0';
                }
                int64_t got = read_dev_tty((char *)kbuf, count, tty_idx);
                if (got >= 0) { write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got); }
                free(kbuf);
                frame->rax = (uint64_t)got;
                return;
            }

            uint8_t *kbuf = malloc(count);
            res = read_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) { write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, res); entry->offset += res; }
            free(kbuf);
        } else if (is_mounted_under(entry->path, "devpts", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
            uint8_t *kbuf = malloc(count);
            res = read_pts_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) { write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, res); entry->offset += res; }
            free(kbuf);
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
        if (got >= 0) {
            write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got);
        }
        free(kbuf);
        frame->rax = (got < 0) ? (uint64_t)-EBADF : (uint64_t)got;
        return;
    }

    if (entry->type == FD_PIPE || entry->type == FD_SOCKET) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t got = read_unix_handle((unix_handle_t *)entry->handle, kbuf, count, entry->flags);
        if (got >= 0) {
            write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, got);
        }
        free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }

    if (entry->type == FD_PROC) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!procfs_resolve(entry->path, self, &n)) { frame->rax = (uint64_t)-ENOENT; return; }
        // Directories and symlinks have no readable byte stream.
        if (procfs_is_dir(&n) || n.type == PROC_NODE_SYMLINK) {
            frame->rax = 0;
            return;
        }
        char tmp[PROCFS_MAX_CONTENT];
        size_t len = procfs_content(&n, tmp);
        if (entry->offset >= len) { frame->rax = 0; return; }
        uint64_t avail = len - entry->offset;
        uint64_t to_read = (count < avail) ? count : avail;
        write_vmm(current_task_ptr->ctx, (uint64_t)buf, (uint8_t *)tmp + entry->offset, to_read);
        entry->offset += to_read;
        frame->rax = to_read;
        return;
    }

    rootfs_file_t file = read_rootfs(entry->path);
    if (!file.data || entry->offset >= file.size) { frame->rax = 0; return; }

    uint64_t avail = file.size - entry->offset;
    uint64_t to_read = (count < avail) ? count : avail;
    write_vmm(current_task_ptr->ctx, (uint64_t)buf, (uint8_t *)file.data + entry->offset, to_read);
    entry->offset += to_read;
    frame->rax = to_read;
}

void sys_write(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const uint8_t *buf = (const uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    // Validate user buffer pointer
    if (!buf) { frame->rax = (uint64_t)-EINVAL; return; }
    if (count > 0 && !user_ptr_ok(buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_STREAM) {
        char kbuf[256];
        uint64_t processed = 0;
        while (processed < count) {
            uint64_t chunk = count - processed;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf + processed, chunk);
            for (uint64_t i = 0; i < chunk; i++) {
                putchar(kbuf[i]);
            }
            processed += chunk;
        }
        frame->rax = count;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (is_mounted_under(entry->path, "devtmpfs", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
            uint8_t *kbuf = malloc(count);
            if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
            read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count);
            res = write_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) entry->offset += res;
            free(kbuf);
        } else if (is_mounted_under(entry->path, "devpts", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
            uint8_t *kbuf = malloc(count);
            if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
            read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count);
            res = write_pts_device(rel, kbuf, count, entry->offset);
            if ((int64_t)res >= 0) entry->offset += res;
            free(kbuf);
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
        read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count);
        int w = write_pty_master(idx, (const char *)kbuf, (int)count);
        free(kbuf);
        frame->rax = (w < 0) ? (uint64_t)-EBADF : (uint64_t)w;
        return;
    }

    if (entry->type == FD_PIPE || entry->type == FD_SOCKET) {
        if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, count);
        int64_t w = write_unix_handle((unix_handle_t *)entry->handle, kbuf, count, entry->flags);
        free(kbuf);
        frame->rax = (uint64_t)w;
        return;
    }

    rootfs_file_t file = read_rootfs(entry->path);
    if (!can_access_rootfs(&file, 0, 1, 0)) { frame->rax = (uint64_t)-EACCES; return; }

    uint64_t new_size = entry->offset + count;
    if (file.size > new_size) new_size = file.size;

    void *new_data = malloc(new_size);
    if (!new_data) { frame->rax = (uint64_t)-ENOMEM; return; }

    if (file.data && file.size) memcpy(new_data, file.data, file.size);
    read_vmm(current_task_ptr->ctx, (uint8_t *)new_data + entry->offset, (uint64_t)buf, count);

    int res = write_rootfs(entry->path, new_data, new_size,
                           file.mode ? file.mode : 0644,
                           file.mode ? file.uid : current_task_ptr->euid,
                           file.mode ? file.gid : current_task_ptr->egid);
    free(new_data);

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
    int cr = copy_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));
    strncpy(abs_path, resolved, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';

    char rel_path[256];
    if (is_mounted_under(abs_path, "devtmpfs", rel_path)) {
        if (rel_path[0] != '\0' && !devtmpfs_device_exists(rel_path)) {
            rootfs_file_t file = read_rootfs(abs_path);
            if (!(file.mode & 0040000)) {
                frame->rax = (uint64_t)-ENOENT;
                return;
            }
        }
        // ptmx: allocate a PTY and return a master fd
        if (0) {
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
    } else if (is_mounted_under(abs_path, "devpts", rel_path)) {
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
            rootfs_file_t file = read_rootfs(abs_path);
            if (!(file.mode & 0040000)) {
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

    // procfs: /proc, /proc/self, /proc/<pid>, /proc/<pid>/{maps,mounts,exe,...}
    {
        int pr = proc_open_common(abs_path, flags);
        if (pr != 1) { frame->rax = (uint64_t)pr; return; }
    }

    rootfs_file_t file = read_rootfs(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(abs_path, "", 0, mode | 0o100000, current_task_ptr->euid, current_task_ptr->egid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
        file = read_rootfs(abs_path);
    }

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);
    if (!can_access_rootfs(&file, want_read, want_write, 0)) { frame->rax = (uint64_t)-EACCES; return; }

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
    if (!user_ptr_ok(st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int cr = copy_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst)) {
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }
    if (stat_proc(abs_path, path, &kst, true)) {
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }

    rootfs_file_t file = read_rootfs(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    kst.st_mode = file.mode;
    kst.st_uid = file.uid;
    kst.st_gid = file.gid;
    kst.st_size = file.size;
    kst.st_blocks = (file.size + 511) / 512;
    kst.st_blksize = 4096;
    kst.st_nlink = 1;

    write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
    frame->rax = 0;
}

void sys_lstat(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    struct stat *st = (struct stat *)frame->rsi;

    if (!user_path || !st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int cr = copy_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst)) {
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }
    if (stat_proc(abs_path, path, &kst, false)) {
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }

    rootfs_file_t file = stat_rootfs_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    kst.st_mode = file.mode;
    kst.st_uid = file.uid;
    kst.st_gid = file.gid;
    kst.st_size = file.size;
    kst.st_blocks = (file.size + 511) / 512;
    kst.st_blksize = 4096;
    kst.st_nlink = 1;

    write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
    frame->rax = 0;
}

void sys_fstat(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    struct stat *st = (struct stat *)frame->rsi;

    if (!st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type == FD_DEV) {
        struct stat kst = {0};
        if (stat_virtual_device(entry->path, &kst)) {
            write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
            frame->rax = 0;
            return;
        }
    }

    // FD_STREAM fds (init's stdin/stdout/stderr) point at /dev/tty0: stat them
    // as that character device so fstat()/ttyname() behave correctly.
    if (entry->type == FD_STREAM) {
        struct stat kst = {0};
        if (stat_virtual_device("/dev/tty0", &kst)) {
            write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
            frame->rax = 0;
            return;
        }
    }

    if (entry->type == FD_FILE) {
        rootfs_file_t file = read_rootfs(entry->path);
        struct stat kst = {0};
        kst.st_mode = file.mode;
        kst.st_uid = file.uid;
        kst.st_gid = file.gid;
        kst.st_size = file.size;
        kst.st_blocks = (file.size + 511) / 512;
        kst.st_blksize = 4096;
        kst.st_nlink = 1;
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }
    if (entry->type == FD_PROC) {
        struct stat kst = {0};
        if (stat_proc(entry->path, NULL, &kst, true)) {
            write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
            frame->rax = 0;
            return;
        }
    }
    frame->rax = (uint64_t)-EBADF;
}

void sys_fstatat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    struct stat *st = (struct stat *)frame->rdx;
    int flags = (int)frame->r10;

    if (!user_path || !st) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(st, sizeof(struct stat))) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    int cr = copy_from_user(path, user_path, sizeof(path));
    if (cr < 0) { frame->rax = (uint64_t)cr; return; }

    char abs_path[256];
    int br = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (br < 0) { frame->rax = (uint64_t)br; return; }

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst)) {
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }
    if (stat_proc(abs_path, path, &kst, (flags & AT_SYMLINK_NOFOLLOW) == 0)) {
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }

    rootfs_file_t file;
    if (flags & AT_SYMLINK_NOFOLLOW) {
        file = stat_rootfs_nofollow(abs_path);
    } else {
        file = read_rootfs(abs_path);
    }
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    kst.st_mode = file.mode;
    kst.st_uid = file.uid;
    kst.st_gid = file.gid;
    kst.st_size = file.size;
    kst.st_blocks = (file.size + 511) / 512;
    kst.st_blksize = 4096;
    kst.st_nlink = 1;

    write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
    frame->rax = 0;
}

void sys_lseek(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int64_t offset = (int64_t)frame->rsi;
    int whence = (int)frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry || !entry->open) { frame->rax = -EBADF; return; }
    if (entry->type == FD_STREAM || entry->type == FD_PIPE || entry->type == FD_SOCKET || entry->type == FD_DEV) { 
        frame->rax = -ESPIPE; 
        return; 
    }

    // Compute the file size for SEEK_END.  Proc files have no rootfs backing.
    int64_t file_size = -1;
    if (entry->type == FD_PROC) {
        int self = proc_self_idx();
        proc_node_t n;
        if (procfs_resolve(entry->path, self, &n) && !procfs_is_dir(&n) &&
            n.type != PROC_NODE_SYMLINK) {
            char tmp[PROCFS_MAX_CONTENT];
            file_size = (int64_t)procfs_content(&n, tmp);
        } else {
            file_size = 0;  // dirs and symlinks: SEEK_END lands at 0
        }
    } else {
        rootfs_file_t file = read_rootfs(entry->path);
        if (!file.mode) { frame->rax = -ENOENT; return; }
        file_size = (int64_t)file.size;
    }

    switch (whence) {
        case 0: // SEEK_SET
            // Reject negative absolute positions
            if (offset < 0) { frame->rax = -EINVAL; return; }
            entry->offset = (uint64_t)offset;
            break;
        case 1: { // SEEK_CUR
            // Guard against signed overflow and underflow
            int64_t cur = (int64_t)entry->offset;
            if (offset > 0 && cur > (int64_t)0x7FFFFFFFFFFFFFFFLL - offset) { frame->rax = -EOVERFLOW; return; }
            int64_t new_off = cur + offset;
            if (new_off < 0) { frame->rax = -EINVAL; return; }
            entry->offset = (uint64_t)new_off;
            break;
        }
        case 2: { // SEEK_END
            int64_t new_off = file_size + offset;
            if (new_off < 0) { frame->rax = -EINVAL; return; }
            entry->offset = (uint64_t)new_off;
            break;
        }
        default:
            frame->rax = -EINVAL;
            return;
    }

    frame->rax = (uint64_t)entry->offset;
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

    // Validate addr if MAP_FIXED or hint provided
    if (addr != 0 && !user_addr_ok(addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }
    if ((flags & MAP_FIXED) && (addr & (PAGE_SIZE - 1))) { frame->rax = (uint64_t)-EINVAL; return; }

    // Reject W+X mappings (W^X policy)
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        frame->rax = (uint64_t)-EACCES; return;
    }

    uint64_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    // Overflow check: ensure num_pages * PAGE_SIZE doesn't wrap
    if (num_pages > (USER_ADDR_MAX / PAGE_SIZE)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t vmm_flags = VMM_USER;
    if (prot & PROT_WRITE) vmm_flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_NX;
    if (flags & MAP_SHARED) vmm_flags |= VMM_SHARED;

    void *ptr = NULL;
    if (flags & MAP_FIXED) {
        ptr = vmap_user_at(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE, vmm_flags);
    } else if (addr != 0) {
        // Hint: try to map at addr, if fails, fallback to auto
        ptr = vmap_user_at(current_task_ptr->ctx, addr & ~(PAGE_SIZE - 1), num_pages * PAGE_SIZE, vmm_flags);
        if (!ptr) ptr = vmap_user_range(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    } else {
        ptr = vmap_user_range(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    }

    if (!ptr) {
        frame->rax = (uint64_t)-ENOMEM; return;
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
        if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
            fd_entry_t *mentry = get_current_fd(fd);
            if (mentry) {
                strncpy(namebuf, mentry->path, sizeof(namebuf) - 1);
                namebuf[sizeof(namebuf) - 1] = '\0';
                name = namebuf;
            }
        }
        vma_add(&current_task_ptr->vmas, (uint64_t)ptr,
                (uint64_t)ptr + num_pages * PAGE_SIZE, vprot, vflags, offset, name);
    }

    // File-backed mapping: copy data if not anonymous
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        fd_entry_t *entry = get_current_fd(fd);
        if (!entry) {
            // Should probably munmap here if we were strict
            frame->rax = (uint64_t)-EBADF;
            return;
        }

        rootfs_file_t file = read_rootfs(entry->path);
        if (file.data) {
            // Validate offset is within file bounds to prevent out-of-bounds read
            if (offset >= file.size && file.size > 0) {
                // Nothing to copy, mapping is zero-filled
            } else {
                uint64_t map_size = num_pages * PAGE_SIZE;
                uint64_t file_avail = (file.size > offset) ? (file.size - offset) : 0;
                uint64_t copy_len = (file_avail < map_size) ? file_avail : map_size;
                if (copy_len > 0) { write_vmm(current_task_ptr->ctx, (uint64_t)ptr, (uint8_t *)file.data + offset, copy_len); }
            }
            // Zero the remaining bytes (BSS-like) is already handled by vmap_user_at/vmalloc_ex
            // which zeroed the newly allocated pages.
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
    if (!user_addr_ok(addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        frame->rax = (uint64_t)-EACCES; return;
    }

    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (addr + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

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
        vma_protect(&current_task_ptr->vmas, start, end, vprot);
    }

    frame->rax = 0;
}

void sys_munmap(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;

    if (length == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    // Ensure entire range is in user-space
    if (!user_addr_ok(addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (addr + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        unmap_vmm(current_task_ptr->ctx, a);
    }

    // Drop the now-unmapped range from the VMA table.
    vma_remove(&current_task_ptr->vmas, start, end);

    frame->rax = 0;
}

// Maximum heap size: 256 MiB per process
#define MAX_BRK_SIZE (256ULL * 1024ULL * 1024ULL)

void sys_brk(syscall_frame_t *frame) {
    uint64_t addr = frame->rdi;

    if (addr == 0) {
        // Return current break
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Validate: new brk must be in user-space and above brk_start
    if (!user_addr_ok(addr, 1)) { frame->rax = current_task_ptr->brk; return; }
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
    uint64_t new_brk = (addr + 0xFFF) & ~0xFFFULL;

    if (new_brk > old_brk) {
        // Map new pages
        for (uint64_t a = old_brk; a < new_brk; a += 4096) {
            if (get_vmm_phys(current_task_ptr->ctx, a) == 0) {
                void *page = pmalloc();
                if (!page) {
                    // OOM - return current brk without updating
                    frame->rax = current_task_ptr->brk;
                    return;
                }
                map_vmm(current_task_ptr->ctx, a, (uint64_t)page,
                        VMM_USER | VMM_WRITABLE | VMM_NX);
                memset_vmm(current_task_ptr->ctx, a, 0, 4096);
            }
        }
    }

    current_task_ptr->brk = addr;
    // Keep the [heap] VMA in sync with the break so /proc/<pid>/maps is accurate.
    vma_set_heap(&current_task_ptr->vmas, current_task_ptr->brk_start, current_task_ptr->brk);
    frame->rax = current_task_ptr->brk;
}

void sys_rt_sigaction(syscall_frame_t *frame) {
    int signum = (int)frame->rdi;
    uint64_t act_ptr = frame->rsi;
    uint64_t oldact_ptr = frame->rdx;

    if (signum < 1 || signum > 31) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    if (oldact_ptr) {
        write_vmm(current_task_ptr->ctx, oldact_ptr, &current_task_ptr->sigactions[signum * 4], 32);
    }
    if (act_ptr) {
        read_vmm(current_task_ptr->ctx, &current_task_ptr->sigactions[signum * 4], act_ptr, 32);
    }
    frame->rax = 0;
}

void sys_rt_sigreturn(syscall_frame_t *frame) {
    uint64_t user_rsp = frame->rsp;
    syscall_frame_t saved_frame;
    read_vmm(current_task_ptr->ctx, &saved_frame, user_rsp, sizeof(syscall_frame_t));
    *frame = saved_frame;
}

void sys_ioctl(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    unsigned long req = (unsigned long)frame->rsi;
    uint64_t argp = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);

    // Handle framebuffer ioctl requests
    if (entry && entry->type == FD_DEV) {
        char rel[256];
        if (is_mounted_under(entry->path, "devtmpfs", rel)) {
            if (strncmp(rel, "fb", 2) == 0) {
                int idx = rel[2] - '0';
                if (fb_req.response && idx >= 0 && idx < (int)fb_req.response->framebuffer_count) {
                    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
                    if (req == FBIOGET_VSCREENINFO) {
                        struct fb_var_screeninfo vinfo;
                        memset(&vinfo, 0, sizeof(vinfo));
                        vinfo.xres = fb->width;
                        vinfo.yres = fb->height;
                        vinfo.xres_virtual = fb->width;
                        vinfo.yres_virtual = fb->height;
                        vinfo.bits_per_pixel = fb->bpp;
                        write_vmm(current_task_ptr->ctx, argp, &vinfo, sizeof(vinfo));
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOGET_FSCREENINFO) {
                        struct fb_fix_screeninfo finfo;
                        memset(&finfo, 0, sizeof(finfo));
                        strncpy(finfo.id, "limine-fb", 15);
                        finfo.line_length = fb->pitch;
                        finfo.smem_len = fb->height * fb->pitch;
                        write_vmm(current_task_ptr->ctx, argp, &finfo, sizeof(finfo));
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
            if (is_mounted_under(entry->path, "devtmpfs", rel)) {
                if (strncmp(rel, "tty", 3) == 0 || strncmp(rel, "pts/", 4) == 0 || strcmp(rel, "console") == 0) is_tty = 1;
            } else if (is_mounted_under(entry->path, "devpts", rel)) {
                is_tty = 1;
            }
        }
    }

    switch (req) {
        case TIOCGWINSZ: {
            winsize_t ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };
            write_vmm(current_task_ptr->ctx, argp, &ws, sizeof(ws));
            frame->rax = 0;
            return;
        }
        case TIOCSWINSZ:
            // Accept but ignore window size sets
            frame->rax = 0;
            return;
        case TCGETS: {
            struct termios t = {0};
            int idx = current_task_ptr->ctty_idx; // Dynamically grab the task's assigned TTY

            // FD_STREAM fds (init's stdin/stdout/stderr) point at tty0 but
            // carry a literal path; resolve them to tty0 so tcgetattr() works.
            if (entry && (entry->type == FD_DEV || entry->type == FD_STREAM)) {
                char rel[256];
                if (entry->type == FD_STREAM) {
                    idx = 0;
                } else if (is_mounted_under(entry->path, "devtmpfs", rel) && strncmp(rel, "tty", 3) == 0) {
                     int path_idx = rel[3] - '0';
                     if (path_idx >= 0 && path_idx < NUM_TTYS) { idx = path_idx; }
                }
            }
        
            // Grab the actual, live configuration instead of a hardcoded constant mask
            tty_t *tty_tcgets = get_tty(idx);
            if (!tty_tcgets) { frame->rax = -ENOTTY; return; }
            t = tty_tcgets->termios;
        
            write_vmm(current_task_ptr->ctx, argp, &t, sizeof(t));
            frame->rax = 0;
            return;
        }
    
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            int idx = current_task_ptr->ctty_idx; // Default to the process's controlling terminal

            // FD_STREAM fds behave as /dev/tty0 (see TCGETS above).
            if (entry && (entry->type == FD_DEV || entry->type == FD_STREAM)) {
                char rel[256];
                if (entry->type == FD_STREAM) {
                    idx = 0;
                } else if (is_mounted_under(entry->path, "devtmpfs", rel) && strncmp(rel, "tty", 3) == 0) {
                    int path_idx = rel[3] - '0';
                    if (path_idx >= 0 && path_idx < NUM_TTYS) { idx = path_idx; }
                }
            }
        
            // Save the modifications to the exact terminal state this task uses
            tty_t *tty_tcsets = get_tty(idx);
            if (!tty_tcsets) { frame->rax = -ENOTTY; return; }
            read_vmm(current_task_ptr->ctx, &tty_tcsets->termios, argp, sizeof(struct termios));
        
            frame->rax = 0;
            return;
        case TCFLSH:
            if (entry && entry->type == FD_DEV) {
                char rel[256];
                if (is_mounted_under(entry->path, "devtmpfs", rel)) {
                    if (strncmp(rel, "tty", 3) == 0) {
                        int idx = rel[3] - '0';
                        if (idx >= 0 && idx < NUM_TTYS) {
                            if (argp == 0  || argp == 2 ) {
                                get_tty(idx)->input.head = get_tty(idx)->input.tail = 0;
                            }
                        }
                    }
                } else if (is_mounted_under(entry->path, "devpts", rel)) {
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
        case TCSBRK:
        case TCXONC:
            frame->rax = 0;
            return;

        case TIOCSCTTY: {
            // Claim this tty as the calling process's controlling terminal
            // Only session leaders can do this
            // Determine TTY index from the fd
            int tidx = 0;
            if (entry) {
                if (entry->type == FD_STREAM) {
                    // Parse from path
                    const char *p = strstr(entry->path, "tty");
                    if (p && p[3] >= '0' && p[3] <= '7') {
                        tidx = p[3] - '0';
                    }
                } else if (entry->type == FD_DEV) {
                    char rel[256];
                    if (is_mounted_under(entry->path, "devtmpfs", rel) && strncmp(rel, "tty", 3) == 0) {
                        if (strlen(rel) > 3 && rel[3] >= '0' && rel[3] <= '7') {
                            tidx = rel[3] - '0';
                        }
                    }
                }
            }
            tty_t *tty_ct = get_tty(tidx);
            if (tty_ct) {
                current_task_ptr->ctty_idx = tidx;
                // POSIX: when acquiring a controlling terminal, the process group
                // of the calling process becomes the foreground process group
                tty_ct->fg_pgrp = current_task_ptr->pgid;
                // Set keyboard input to go to this TTY
                set_keyboard_tty(tidx);
            }
            frame->rax = 0;
            return;
        }

        case TIOCGPGRP: {
            // Return the foreground process group of the controlling tty
            if (current_task_ptr->ctty_idx < 0) {
                frame->rax = (uint64_t)-ENOTTY;
                return;
            }
            tty_t *tty_fg = get_tty(current_task_ptr->ctty_idx);
            pid_t pgrp = tty_fg ? tty_fg->fg_pgrp : current_task_ptr->pgid;
            if (pgrp == 0) pgrp = current_task_ptr->pgid;
            write_vmm(current_task_ptr->ctx, argp, &pgrp, sizeof(pid_t));
            frame->rax = 0;
            return;
        }

        case TIOCSPGRP: {
            // Set the foreground process group of the controlling tty
            pid_t new_pgrp = 0;
            read_vmm(current_task_ptr->ctx, &new_pgrp, argp, sizeof(pid_t));
        
            // Must have a controlling terminal
            if (current_task_ptr->ctty_idx < 0) {
                frame->rax = (uint64_t)-ENOTTY;
                return;
            }
        
            tty_t *tty_sp = get_tty(current_task_ptr->ctty_idx);
            if (!tty_sp) {
                frame->rax = (uint64_t)-ENOTTY;
                return;
            }
        
            // POSIX: only session leader or process with matching pgid can set fg_pgrp
            if (current_task_ptr->pid != current_task_ptr->sid && 
                new_pgrp != current_task_ptr->pgid) {
                frame->rax = (uint64_t)-EPERM;
                return;
            }
        
            tty_sp->fg_pgrp = new_pgrp;
            frame->rax = 0;
            return;
        }

        case FIONREAD: {
            // Report bytes available in per-task stdin buffer; 0 for everything else
            uint64_t irq;
            spin_lock_irqsave(&stdin_lock, &irq);
            int avail = (fd == 0) ? (current_task_ptr->stdin_buf_len - current_task_ptr->stdin_buf_pos) : 0;
            spin_unlock_irqrestore(&stdin_lock, irq);
            write_vmm(current_task_ptr->ctx, argp, &avail, sizeof(int));
            frame->rax = 0;
            return;
        }

        case TIOCEXCL:
        case TIOCNXCL:
            // Exclusive mode, no-op
            frame->rax = 0;
            return;

        case TIOCNOTTY:
            current_task_ptr->ctty_idx = -1;
            frame->rax = 0;
            return;

        case TIOCGPTN: {
            if (!entry || entry->type != FD_PTY_MASTER) { frame->rax = (uint64_t)-ENOTTY; return; }
            int idx = ptm_path_idx(entry->path);
            unsigned int uidx = (unsigned int)idx;
            write_vmm(current_task_ptr->ctx, argp, &uidx, sizeof(unsigned int));
            frame->rax = 0;
            return;
        }
        case TIOCSPTLCK: {
            if (!entry || entry->type != FD_PTY_MASTER) { frame->rax = (uint64_t)-ENOTTY; return; }
            int idx = ptm_path_idx(entry->path);
            pty_t *p = get_pty(idx);
            if (!p) { frame->rax = (uint64_t)-EBADF; return; }
            int val = 0;
            read_vmm(current_task_ptr->ctx, &val, argp, sizeof(int));
            p->locked = (val != 0);
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

    if (count > 0 && !user_ptr_ok(buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_FILE) { frame->rax = (uint64_t)-ESPIPE; return; }

    rootfs_file_t file = read_rootfs(entry->path);
    if (!file.data || offset >= file.size) {
        frame->rax = 0; return;
    }

    uint64_t avail = file.size - offset;
    uint64_t to_read = (count < avail) ? count : avail;
    write_vmm(current_task_ptr->ctx, (uint64_t)buf, (uint8_t *)file.data + offset, to_read);
    frame->rax = to_read;
}

void sys_readv(syscall_frame_t *frame) {
    const struct iovec *uiov = (const struct iovec *)frame->rsi;
    int iovcnt = (int)frame->rdx;

    if (iovcnt < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (iovcnt == 0) { frame->rax = 0; return; }
    if (!uiov) { frame->rax = (uint64_t)-EFAULT; return; }
    if (iovcnt > MAX_IOV) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(uiov, (uint64_t)iovcnt * sizeof(struct iovec))) { frame->rax = (uint64_t)-EFAULT; return; }

    struct iovec kiov[MAX_IOV];
    read_vmm(current_task_ptr->ctx, kiov, (uint64_t)uiov, (uint64_t)iovcnt * sizeof(struct iovec));

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
    if (!user_ptr_ok(uiov, (uint64_t)iovcnt * sizeof(struct iovec))) { frame->rax = (uint64_t)-EFAULT; return; }

    struct iovec kiov[MAX_IOV];
    read_vmm(current_task_ptr->ctx, kiov, (uint64_t)uiov, (uint64_t)iovcnt * sizeof(struct iovec));

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

void sys_sendfile(syscall_frame_t *frame) {
    int out_fd = (int)frame->rdi;
    int in_fd = (int)frame->rsi;
    int64_t *offset_ptr = (int64_t *)frame->rdx;
    size_t count = (size_t)frame->r10;

    fd_entry_t *in_entry = get_current_fd(in_fd);
    if (!in_entry) { frame->rax = (uint64_t)-EBADF; return; }
    fd_entry_t *out_entry = get_current_fd(out_fd);
    if (!out_entry) { frame->rax = (uint64_t)-EBADF; return; }

    // Only support file-to-file for now
    if (in_entry->type != FD_FILE || out_entry->type != FD_FILE) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    rootfs_file_t in_file = read_rootfs(in_entry->path);
    if (!in_file.data) { frame->rax = (uint64_t)-EBADF; return; }

    int64_t offset = 0;
    if (offset_ptr) {
        if (!user_ptr_ok(offset_ptr, sizeof(int64_t))) { frame->rax = (uint64_t)-EFAULT; return; }
        read_vmm(current_task_ptr->ctx, &offset, (uint64_t)offset_ptr, sizeof(int64_t));
    } else {
        offset = (int64_t)in_entry->offset;
    }

    if (offset < 0 || (uint64_t)offset > in_file.size) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t avail = in_file.size - (uint64_t)offset;
    uint64_t to_copy = (count < avail) ? count : avail;

    if (to_copy == 0) { frame->rax = 0; return; }

    // Read source data from rootfs
    rootfs_file_t out_file = read_rootfs(out_entry->path);
    uint64_t new_size = out_entry->offset + to_copy;
    if (out_file.size > new_size) new_size = out_file.size;

    void *new_data = malloc(new_size);
    if (!new_data) { frame->rax = (uint64_t)-ENOMEM; return; }

    if (out_file.data && out_file.size)
        memcpy(new_data, out_file.data, out_file.size);
    memcpy((uint8_t *)new_data + out_entry->offset, (uint8_t *)in_file.data + (uint64_t)offset, to_copy);

    int res = write_rootfs(out_entry->path, new_data, new_size,
                           out_file.mode ? out_file.mode : 0644,
                           out_file.mode ? out_file.uid : current_task_ptr->euid,
                           out_file.mode ? out_file.gid : current_task_ptr->egid);
    free(new_data);

    if (res < 0) { frame->rax = (uint64_t)res; return; }

    out_entry->offset += to_copy;
    offset += (int64_t)to_copy;

    if (offset_ptr)
        write_vmm(current_task_ptr->ctx, (uint64_t)offset_ptr, &offset, sizeof(int64_t));
    else
        in_entry->offset = (uint64_t)offset;

    frame->rax = to_copy;
}

void sys_pipe(syscall_frame_t *frame) {
    int *pipefd = (int *)frame->rdi;
    unix_handle_t *read_end = NULL;
    unix_handle_t *write_end = NULL;
    int fds[2];
    int r;

    if (!pipefd) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(pipefd, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
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

    write_vmm(current_task_ptr->ctx, (uint64_t)pipefd, fds, sizeof(fds));
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
    if (table->entries[newfd].open)
        free_fd(table, newfd);

    table->entries[newfd] = *src;
    table->entries[newfd].open = true;
    retain_fd_entry(&table->entries[newfd]);
    frame->rax = (uint64_t)newfd;
}

void sys_nanosleep(syscall_frame_t *frame) {
    struct timespec *req = (struct timespec *)frame->rdi;
    struct timespec *rem = (struct timespec *)frame->rsi;
    
    if (!req) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(req, sizeof(struct timespec))) { frame->rax = (uint64_t)-EFAULT; return; }

    struct timespec ts;
    read_vmm(current_task_ptr->ctx, &ts, (uint64_t)req, sizeof(struct timespec));

    // Validate: tv_nsec must be in [0, 999999999] and tv_sec must be non-negative
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L || ts.tv_sec < 0) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    
    // Cap sleep at ~292 years to prevent integer overflow in µs calculation
    if (ts.tv_sec > 9000000000LL) { ts.tv_sec = 9000000000LL; }
    
    uint64_t total_us = ((uint64_t)ts.tv_sec * 1000000ULL) + (uint64_t)(ts.tv_nsec / 1000);
    uint64_t start_time = hpet_elapsed_us();

    // Loop until the requested time has actually elapsed
    while (hpet_elapsed_us() - start_time < total_us) {
        if (signal_pending()) {
            if (rem) {
                uint64_t elapsed = hpet_elapsed_us() - start_time;
                uint64_t remaining_us = total_us > elapsed ? total_us - elapsed : 0;
                struct timespec r = { 
                    .tv_sec = remaining_us / 1000000, 
                    .tv_nsec = (remaining_us % 1000000) * 1000 
                };
                if (user_ptr_ok(rem, sizeof(struct timespec))) {
                    write_vmm(current_task_ptr->ctx, (uint64_t)rem, &r, sizeof(struct timespec));
                }
            }
            frame->rax = (uint64_t)-EINTR;
            return;
        }

        current_task_ptr->state = TASK_READY;
        
        spin_unlock(&sched_lock);
        __asm__ volatile("int $32"); 
        spin_lock(&sched_lock);
    }

    if (rem) {
        if (!user_ptr_ok(rem, sizeof(struct timespec))) { frame->rax = (uint64_t)-EFAULT; return; }
        struct timespec zero_ts = {0, 0};
        write_vmm(current_task_ptr->ctx, (uint64_t)rem, &zero_ts, sizeof(struct timespec));
    }

    frame->rax = 0;
}

void sys_getpid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_socket(syscall_frame_t *frame) {
    int domain = (int)frame->rdi;
    int type = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    unix_handle_t *h = NULL;
    int r = create_unix_socket(domain, type, protocol, &h);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    int fd = alloc_fd_handle(&current_task_ptr->fd_table, "socket", FD_SOCKET, O_RDWR, h);
    if (fd < 0) {
        release_unix_handle(h);
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
    if (addrlen > 0 && !user_ptr_ok(addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
    // Copy sockaddr to kernel buffer for safe access
    sockaddr_un_t kaddr;
    memset(&kaddr, 0, sizeof(kaddr));
    uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
    read_vmm(current_task_ptr->ctx, &kaddr, (uint64_t)addr, copy_len);
    frame->rax = (uint64_t)connect_unix_socket((unix_handle_t *)entry->handle, &kaddr, copy_len);
}

void sys_accept(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    fd_entry_t *entry = get_current_fd(fd);
    unix_handle_t *accepted = NULL;
    int r;
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }

    r = accept_unix_socket((unix_handle_t *)entry->handle, &accepted);
    if (r < 0) { frame->rax = (uint64_t)r; return; }
    int newfd = alloc_fd_handle(&current_task_ptr->fd_table, "socket:accepted", FD_SOCKET, O_RDWR, accepted);
    if (newfd < 0) {
        release_unix_handle(accepted);
        frame->rax = (uint64_t)newfd;
        return;
    }
    frame->rax = (uint64_t)newfd;
}

void sys_sendto(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (len > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
    if (len > 0 && !user_ptr_ok(buf, len)) { frame->rax = (uint64_t)-EFAULT; return; }
    // Copy user data to kernel buffer before sending
    uint8_t *kbuf = malloc(len);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
    read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, len);
    int64_t w = write_unix_handle((unix_handle_t *)entry->handle, kbuf, len, entry->flags);
    free(kbuf);
    frame->rax = (uint64_t)w;
}

void sys_recvfrom(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (len > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
    if (len > 0 && !user_ptr_ok(buf, len)) { frame->rax = (uint64_t)-EFAULT; return; }
    // Read into kernel buffer first, then copy to user
    uint8_t *kbuf = malloc(len);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
    int64_t got = read_unix_handle((unix_handle_t *)entry->handle, kbuf, len, entry->flags);
    if (got > 0) {
        write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (size_t)got);
    }
    free(kbuf);
    frame->rax = (uint64_t)got;
}

void sys_shutdown(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int how = (int)frame->rsi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    frame->rax = (uint64_t)shutdown_unix_socket((unix_handle_t *)entry->handle, how);
}

void sys_bind(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *addr = (const void *)frame->rsi;
    uint32_t addrlen = (uint32_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (addrlen > 0 && !user_ptr_ok(addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
    // Copy sockaddr to kernel buffer for safe access
    sockaddr_un_t kaddr;
    memset(&kaddr, 0, sizeof(kaddr));
    uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
    read_vmm(current_task_ptr->ctx, &kaddr, (uint64_t)addr, copy_len);
    frame->rax = (uint64_t)bind_unix_socket((unix_handle_t *)entry->handle, &kaddr, copy_len);
}

void sys_listen(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int backlog = (int)frame->rsi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    frame->rax = (uint64_t)listen_unix_socket((unix_handle_t *)entry->handle, backlog);
}

void sys_socketpair(syscall_frame_t *frame) {
    int domain = (int)frame->rdi;
    int type = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int *sv = (int *)frame->r10;
    unix_handle_t *a = NULL;
    unix_handle_t *b = NULL;
    int fds[2];
    int r;

    if (!sv) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(sv, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    r = create_unix_socketpair(domain, type, protocol, &a, &b);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "socketpair", FD_SOCKET, O_RDWR, a);
    if (fds[0] < 0) {
        release_unix_handle(a);
        release_unix_handle(b);
        frame->rax = (uint64_t)fds[0];
        return;
    }
    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "socketpair", FD_SOCKET, O_RDWR, b);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_unix_handle(b);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    write_vmm(current_task_ptr->ctx, (uint64_t)sv, fds, sizeof(fds));
    frame->rax = 0;
}

void sys_reboot(syscall_frame_t *frame) {
    unsigned int magic1 = (unsigned int)frame->rdi;
    unsigned int magic2 = (unsigned int)frame->rsi;
    unsigned int cmd    = (unsigned int)frame->rdx;

    if (!current_task_ptr || current_task_ptr->euid != 0) {
        frame->rax = (uint64_t)-EPERM;
        return;
    }
    if (magic1 != LINUX_REBOOT_MAGIC1) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    switch (cmd) {
        case LINUX_REBOOT_CMD_RESTART:
            if (magic2 != LINUX_REBOOT_MAGIC2) { frame->rax = (uint64_t)-EINVAL; return; }
            reboot();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_POWER_OFF:
            if (magic2 != LINUX_REBOOT_MAGIC2) { frame->rax = (uint64_t)-EINVAL; return; }
            poweroff();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_HALT:
            if (magic2 != LINUX_REBOOT_MAGIC2) { frame->rax = (uint64_t)-EINVAL; return; }
            halt();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_RESTART2:
            if (magic2 != LINUX_REBOOT_MAGIC2A) { frame->rax = (uint64_t)-EINVAL; return; }
            reboot();
            __builtin_unreachable();
        case LINUX_REBOOT_CMD_CAD_ON:
        case LINUX_REBOOT_CMD_CAD_OFF:
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_fork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { frame->rax = (uint64_t)-EAGAIN; return; }

    frame->rax = (uint64_t)child_pid;
}

void sys_vfork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { frame->rax = (uint64_t)-EAGAIN; return; }

    current_task_ptr->state = TASK_STOPPED;
    current_task_ptr->waiting_for = child_pid;

    frame->rax = (uint64_t)child_pid;

    spin_unlock(&sched_lock);
    sti();
    __asm__ volatile ("int $32");
    spin_lock(&sched_lock);
    cli();
}

void sys_execve(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    char **user_argv = (char **)frame->rsi;
    char **user_envp = (char **)frame->rdx;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }
    char path_buf[256];

    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    // Check for shebang
    rootfs_file_t probe = read_rootfs(path_buf);
    if (probe.data && probe.size >= 2 &&
        ((char *)probe.data)[0] == '#' && ((char *)probe.data)[1] == '!') {
        // Parse interpreter from shebang line
        char *p = (char *)probe.data + 2;
        while (*p == ' ' || *p == '\t') p++;
        char interp[256] = {0};
        int ii = 0;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && ii < 255)
            interp[ii++] = *p++;
        interp[ii] = '\0';

        if (ii > 0) {
            char **orig_argv = NULL;
            int orig_argc = copy_from_user_strarray(&orig_argv, (const char **)user_argv, 63);
            if (orig_argc < 0) { frame->rax = (uint64_t)orig_argc; return; }
            int new_argc = orig_argc + 1;
            char **new_argv = vmalloc((new_argc + 1) * sizeof(char *));
            if (!new_argv) {
                free_strarray(orig_argv, orig_argc);
                frame->rax = (uint64_t)-ENOMEM; return;
            }
            new_argv[0] = malloc(256);
            new_argv[1] = malloc(256);
            if (!new_argv[0] || !new_argv[1]) {
                if (new_argv[0]) free(new_argv[0]);
                if (new_argv[1]) free(new_argv[1]);
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)-ENOMEM; return;
            }
            memcpy(new_argv[0], interp, ii + 1);
            memcpy(new_argv[1], path_buf, strlen(path_buf) + 1);
            for (int i = 1; i < orig_argc; i++) new_argv[i + 1] = orig_argv[i];
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
                write_msr(MSR_GS_BASE, 0);
                for (int i = 1; i < 32; i++) {
                    uint64_t *sa = &current_task_ptr->sigactions[i * 4];
                    if (sa[0] != 0 && sa[0] != 1) {
                        sa[0] = 0; sa[1] = 0; sa[2] = 0; sa[3] = 0;
                    }
                }
                current_task_ptr->pending_signals = 0;
            }
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
        write_msr(MSR_GS_BASE, 0);
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

    while (1) {
        int found_child = 0;

        for (int i = 0; i < MAX_TASKS; i++) {
            // Only care about children of the current task
            if (!tasks[i].state || tasks[i].ppid != current_task_ptr->pid) continue;

            if (pid > 0 && tasks[i].pid != pid) continue;
            if (pid == 0 && tasks[i].pgid != current_task_ptr->pgid) continue;
            if (pid < -1 && tasks[i].pgid != -pid) continue;

            found_child = 1;

            if (tasks[i].state == TASK_ZOMBIE) {
                // Encode wstatus correctly (Linux wait status encoding)
                int status;
                if (tasks[i].term_sig != 0) {
                    // Killed by signal: low 7 bits = signal number
                    status = tasks[i].term_sig & 0x7f;
                } else {
                    // Normal exit: bits 8-15 = exit code, low byte = 0
                    status = (tasks[i].exit_status & 0xff) << 8;
                }
                if (wstatus) {
                    write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int));
                }
                if (rusage) {
                    struct rusage ru = {0};
                    write_vmm(current_task_ptr->ctx, (uint64_t)rusage, &ru, sizeof(struct rusage));
                }
                pid_t ret = tasks[i].pid;
                tasks[i].state = TASK_DEAD;
                frame->rax = (uint64_t)ret;
                
                // CRITICAL: Unlock before leaving!
                spin_unlock(&sched_lock);
                return;
            }

            if ((options & 2 /* WUNTRACED */) && tasks[i].state == TASK_STOPPED && !tasks[i].stop_reported) {
                // Report stopped child (bits 8-15 = stop signal, low byte = 0x7f)
                int status = (SIGTSTP << 8) | 0x7f;
                if (wstatus) {
                    write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int));
                }
                tasks[i].stop_reported = 1;
                frame->rax = (uint64_t)tasks[i].pid;
                
                // CRITICAL: Unlock before leaving!
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
            spin_unlock(&sched_lock); // CRITICAL: Unlock before leaving!
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
                if (s != SIGCHLD && s != SIGCONT && s != SIGTSTP && s != SIGSTOP && h == 0) {
                    has_custom = 1; break;
                }
            }
            if (has_custom) {
                frame->rax = (uint64_t)-EINTR;
                spin_unlock(&sched_lock); // CRITICAL: Unlock before leaving!
                return;
            }
        }

        // Yield to let the child run.
        current_task_ptr->state = TASK_READY;
        spin_unlock(&sched_lock);
        __asm__ volatile("int $32");
        spin_lock(&sched_lock);
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
            if (tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
                if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i].uid) {
                    frame->rax = (uint64_t)-EPERM; return;
                }
                if (pid == 1 && sig != 0) { frame->rax = (uint64_t)-EPERM; return; }
                if (sig == 0) { frame->rax = 0; return; }
                deliver_sig_to_task(i, sig);
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
            if (tasks[i].state == TASK_DEAD) continue;
            if (tasks[i].pgid != my_pgrp) continue;
            if (sig == 0) { found = 1; continue; }
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i].uid) continue;
            deliver_sig_to_task(i, sig);
            found = 1;
        }
        frame->rax = found ? 0 : (uint64_t)-ESRCH;
        return;
    }

    if (pid == -1) {
        // Send to every process the caller may signal (all except PID 1)
        int found = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_DEAD) continue;
            if (tasks[i].pid == 1 || tasks[i].pid == current_task_ptr->pid) continue;
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i].uid) continue;
            if (sig != 0) deliver_sig_to_task(i, sig);
            found = 1;
        }
        frame->rax = found ? 0 : (uint64_t)-ESRCH;
        return;
    }

    // pid < -1: send to process group -pid
    pid_t target_pgrp = -pid;
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) continue;
        if (tasks[i].pgid != target_pgrp) continue;
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i].uid) continue;
        if (sig != 0) deliver_sig_to_task(i, sig);
        found = 1;
    }
    frame->rax = found ? 0 : (uint64_t)-ESRCH;
}

void sys_getpgid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    if (pid == 0) {
        frame->rax = (uint64_t)current_task_ptr->pgid;
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            frame->rax = (uint64_t)tasks[i].pgid;
            return;
        }
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_uname(syscall_frame_t *frame) {
    uint64_t bufp = frame->rdi;

    if (!bufp) { frame->rax = (uint64_t)-EFAULT; return; }

    struct utsname info = utsname_info;
    get_hostname(info.nodename, sizeof(info.nodename));

    write_vmm(current_task_ptr->ctx, bufp, &info, sizeof(info));
    frame->rax = 0;
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

    if (entry->type != FD_FILE) { frame->rax = (uint64_t)-EBADF; return; }

    int op = operation & ~LOCK_NB;
    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN) { frame->rax = (uint64_t)-EINVAL; return; }
retry:;
    if (op == LOCK_UN) {
        if (entry->handle) {
            flock_obj_t *obj = (flock_obj_t *)entry->handle;
            obj->lock_type = 0;
        }
        frame->rax = 0;
        return;
    }

    bool conflict = false;
    for (int i = 0; i < 128; i++) {
        if (global_flocks[i].used && global_flocks[i].lock_type != 0 &&
            strncmp(global_flocks[i].path, entry->path, 255) == 0) {

            if (&global_flocks[i] == (flock_obj_t *)entry->handle) continue;

            if (op == LOCK_EX || global_flocks[i].lock_type == LOCK_EX) {
                conflict = true;
                break;
            }
        }
    }

    if (conflict) {
        if (operation & LOCK_NB) { frame->rax = (uint64_t)-EAGAIN; return; }
        sleep(10);
        goto retry;
    }

    if (entry->handle == NULL) {
        entry->handle = alloc_flock_obj(entry->path);
        if (!entry->handle) { frame->rax = (uint64_t)-ENOLCK; return; }
    }

    ((flock_obj_t *)entry->handle)->lock_type = op;

    frame->rax = 0;
}

void sys_getdents(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint64_t bufp = frame->rsi;
    uint64_t buflen = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (buflen == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_addr_ok(bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint64_t written = 0;
    int index = (int)entry->offset;

    // Resolve symlinks in the directory path so that accessing a virtual FS
    // through a symlink (e.g. /dev-link -> /dev) still matches the correct mount.
    char resolved_path[256];
    resolve_dir_for_readdir(entry->path, resolved_path, sizeof(resolved_path), NULL, 0);

    // Check if this directory is a virtual device filesystem
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs)
    char rel[256];
    if (is_mounted_under(resolved_path, "devpts", rel)) {
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

    if (is_mounted_under(resolved_path, "devtmpfs", rel)) {
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
        while (devtmpfs_get_device_name(total_devs)) total_devs++;
        // Emit registered devices
        int dev_idx = index - 2;
        while (dev_idx < total_devs) {
            const char *devname = devtmpfs_get_device_name(dev_idx);
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
            if (!get_sub_mount_name(resolved_path, sub_idx, sub_name, sizeof(sub_name))) break;
            if (!emit_dirent(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_DIR, sub_name)) break;
            sub_idx++;
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // procfs directory enumeration: /proc, /proc/<pid>, /proc/<pid>/fd
    if (entry->type == FD_PROC || procfs_is_proc_path(resolved_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!procfs_resolve(resolved_path, self, &n) || !procfs_is_dir(&n)) {
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
            if (!procfs_get_dirent(&n, self, child_index, child, sizeof(child), &child_type)) break;
            if (!emit_dirent(bufp, &written, buflen, (uint64_t)(child_index + 1), (uint64_t)(child_index + 1), child_type, child)) break;
            child_index++;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Normal rootfs enumeration (resolved_path already has symlink resolution)
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
        if (next_rootfs_child(&child_index, resolved_path, child, sizeof(child), &child_type) != 0) break;
        if (!emit_dirent(bufp, &written, buflen, (uint64_t)(child_index + 1), (uint64_t)(child_index + 1), child_type, child)) break;
        child_index++;
        index = child_index + 2;
        entry->offset = index;
    }

    frame->rax = written;
}

void sys_getdents64(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint64_t bufp = frame->rsi;
    uint64_t buflen = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (buflen == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_addr_ok(bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint64_t written = 0;
    int index = (int)entry->offset;

    // Resolve symlinks in the directory path so that accessing a virtual FS
    // through a symlink (e.g. /dev-link -> /dev) still matches the correct mount.
    char resolved_path[256];
    resolve_dir_for_readdir(entry->path, resolved_path, sizeof(resolved_path), NULL, 0);

    // Check if this directory is a virtual device filesystem
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs)
    char rel[256];
    if (is_mounted_under(resolved_path, "devpts", rel)) {
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

    if (is_mounted_under(resolved_path, "devtmpfs", rel)) {
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
        while (devtmpfs_get_device_name(total_devs)) total_devs++;
        // Emit registered devices
        int dev_idx = index - 2;
        while (dev_idx < total_devs) {
            const char *devname = devtmpfs_get_device_name(dev_idx);
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
            if (!get_sub_mount_name(resolved_path, sub_idx, sub_name, sizeof(sub_name))) break;
            if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(index + 1), (uint64_t)(index + 1), DT_DIR, sub_name)) break;
            sub_idx++;
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // procfs directory enumeration: /proc, /proc/<pid>, /proc/<pid>/fd
    if (entry->type == FD_PROC || procfs_is_proc_path(resolved_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!procfs_resolve(resolved_path, self, &n) || !procfs_is_dir(&n)) {
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
            if (!procfs_get_dirent(&n, self, child_index, child, sizeof(child), &child_type)) break;
            if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(child_index + 1), (uint64_t)(child_index + 1), child_type, child)) break;
            child_index++;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Normal rootfs enumeration (resolved_path already has symlink resolution)
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
        if (next_rootfs_child(&child_index, resolved_path, child, sizeof(child), &child_type) != 0) break;
        if (!emit_dirent64(bufp, &written, buflen, (uint64_t)(child_index + 1), (uint64_t)(child_index + 1), child_type, child)) break;
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
    if (!user_addr_ok(bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    size_t cwd_len = strlen(current_task_ptr->cwd) + 1;
    if (cwd_len > buflen) { frame->rax = (uint64_t)-ERANGE; return; }
    
    char cwd_copy[256];
    if (cwd_len > sizeof(cwd_copy)) cwd_len = sizeof(cwd_copy);
    memcpy(cwd_copy, current_task_ptr->cwd, cwd_len);
    write_vmm(current_task_ptr->ctx, bufp, cwd_copy, cwd_len);
    
    frame->rax = cwd_len;
}

void sys_chdir(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));

    // Fully resolve symlinks so cwd is always canonical
    char resolved[256];
    resolve_path_symlinks(abs_path, resolved, sizeof(resolved));

    // procfs directories (e.g. /proc/<pid>, /proc/self, /proc/<pid>/fd) are
    // virtual: they have no rootfs backing, so validate them through the
    // procfs resolver. Without this, `cd /proc/<pid>` fails with ENOENT even
    // though getdents on /proc lists the pid.
    if (procfs_is_proc_path(resolved)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!procfs_resolve(resolved, self, &n)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (!procfs_is_dir(&n)) { frame->rax = (uint64_t)-ENOTDIR; return; }
        strncpy(current_task_ptr->cwd, resolved, 255);
        current_task_ptr->cwd[255] = '\0';
        frame->rax = 0;
        return;
    }

    rootfs_file_t dir = read_rootfs(resolved);
    if (!dir.data && !dir.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if ((dir.mode & 0040000) == 0 && strcmp(resolved, "/") != 0) {
        frame->rax = (uint64_t)-ENOTDIR; return;
    }

    strncpy(current_task_ptr->cwd, resolved, 255);
    current_task_ptr->cwd[255] = '\0';
    frame->rax = 0;
}

void sys_rename(syscall_frame_t *frame) {
    const char *oldpath = (const char *)frame->rdi;
    const char *newpath = (const char *)frame->rsi;
    
    char old[256], new[256];
    if (copy_from_user(old, oldpath, sizeof(old)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_from_user(new, newpath, sizeof(new)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    rootfs_file_t file = read_rootfs(old);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    void *data_copy = NULL;
    if (file.data && file.size > 0) {
        data_copy = malloc(file.size);
        if (!data_copy) { frame->rax = (uint64_t)-ENOMEM; return; }
        memcpy(data_copy, file.data, file.size);
    }

    write_rootfs(new, data_copy, file.size, file.mode, file.uid, file.gid);
    delete_rootfs(old);
    frame->rax = 0;
}

void sys_mkdir(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    frame->rax = (uint64_t)mkdir_rootfs(path_buf, mode, current_task_ptr->euid, current_task_ptr->egid);
}

void sys_rmdir(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));

    rootfs_file_t file = stat_rootfs(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if ((file.mode & 0xF000) != 0x4000) { frame->rax = (uint64_t)-ENOTDIR; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) {frame->rax = (uint64_t)-EPERM; return; }

    int ret = rmdir_rootfs(abs_path);
    frame->rax = ret < 0 ? (uint64_t)ret : 0;
}

void sys_link(syscall_frame_t *frame) {
    const char *oldpath = (const char *)frame->rdi;
    const char *newpath = (const char *)frame->rsi;

    char old[256], new[256];
    if (copy_from_user(old, oldpath, sizeof(old)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_from_user(new, newpath, sizeof(new)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    rootfs_file_t file = read_rootfs(old);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    void *data_copy = NULL;
    if (file.data && file.size > 0) {
        data_copy = malloc(file.size);
        if (!data_copy) { frame->rax = (uint64_t)-ENOMEM; return; }
        memcpy(data_copy, file.data, file.size);
    }

    write_rootfs(new, data_copy, file.size, file.mode, file.uid, file.gid);
    frame->rax = 0;
}

void sys_unlink(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    get_absolute_path(path_buf, abs_path, sizeof(abs_path));

    // Don't follow the final component: unlink must remove the link itself,
    // not its target.
    rootfs_file_t file = stat_rootfs_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    int ret = delete_rootfs(abs_path);
    if (ret < 0) { frame->rax = (uint64_t)-ENOENT; return; }

    frame->rax = 0;
}

void sys_symlink(syscall_frame_t *frame) {
    const char *target = (const char *)frame->rdi;
    const char *linkpath = (const char *)frame->rsi;

    if (!target || !linkpath) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[256], linkpath_buf[256];
    if (copy_from_user(target_buf, target, sizeof(target_buf)) < 0 || copy_from_user(linkpath_buf, linkpath, sizeof(linkpath_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_linkpath_buf[256];
    get_absolute_path(linkpath_buf, abs_linkpath_buf, sizeof(abs_linkpath_buf));

    frame->rax = (uint64_t)symlink_rootfs(target_buf, abs_linkpath_buf, current_task_ptr->euid, current_task_ptr->egid);
}

void sys_readlink(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    char *buf = (char *)frame->rsi;
    size_t bufsiz = (size_t)frame->rdx;

    if (!user_path || !buf || bufsiz == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_ptr_ok(buf, bufsiz)) { frame->rax = (uint64_t)-EFAULT; return; }

    char path[256];
    if (copy_from_user(path, user_path, sizeof(path)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    // procfs symlinks: /proc/<pid>/exe, /proc/<pid>/cwd, /proc/<pid>/fd/<n>
    if (procfs_is_proc_path(abs_path)) {
        int self = proc_self_idx();
        proc_node_t n;
        if (!procfs_resolve_nofollow(abs_path, self, &n)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (n.type != PROC_NODE_SYMLINK) {
            frame->rax = (uint64_t)-EINVAL;  // not a symlink
            return;
        }
        char target[256];
        int tlen = procfs_readlink(&n, self, target, sizeof(target));
        if (tlen < 0) { frame->rax = (uint64_t)-EINVAL; return; }
        size_t ulen = (size_t)tlen;
        if (ulen > bufsiz) ulen = bufsiz;
        write_vmm(current_task_ptr->ctx, (uint64_t)buf, target, ulen);
        frame->rax = (uint64_t)ulen;
        return;
    }

    // Must inspect the symlink itself, not its target.
    rootfs_file_t file = stat_rootfs_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if ((file.mode & 0xF000) != 0xA000) { frame->rax = (uint64_t)-EINVAL; return; }

    size_t len = strlen((const char *)file.data);
    if (len > bufsiz) len = bufsiz;

    write_vmm(current_task_ptr->ctx, (uint64_t)buf, file.data, len);
    frame->rax = (uint64_t)len;
}

void sys_chmod(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    get_absolute_path(path_buf, abs_path, sizeof(abs_path));

    rootfs_file_t file = stat_rootfs(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }
    int ret = chmod_rootfs(abs_path, mode & 0777);

    frame->rax = ret < 0 ? (uint64_t)ret : 0;
}
void sys_fchmod(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    // Permission check: only the file owner or root may chmod
    rootfs_file_t file = read_rootfs(entry->path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) {
        frame->rax = (uint64_t)-EPERM; return;
    }

    frame->rax = (uint64_t)chmod_rootfs(entry->path, mode & 0777);
}

void sys_umask(syscall_frame_t *frame) {
    mode_t mask = (mode_t)frame->rdi;
    mode_t old_mask = current_umask;
    current_umask = mask & 0777;
    frame->rax = (uint64_t)old_mask;
}

void sys_gettimeofday(syscall_frame_t *frame) {
    struct timeval *tv = (struct timeval *)frame->rdi;
    struct timezone *tz = (struct timezone *)frame->rsi;

    if (tv) {
        uint64_t usec = time_get_realtime_us();

        struct timeval ktv;
        ktv.tv_sec = (time_t)(usec / 1000000ULL);
        ktv.tv_usec = (suseconds_t)(usec % 1000000ULL);
        write_vmm(current_task_ptr->ctx, (uint64_t)tv, &ktv, sizeof(ktv));
    }

    if (tz) { struct timezone ktz = {0}; write_vmm(current_task_ptr->ctx, (uint64_t)tz, &ktz, sizeof(ktz)); }

    frame->rax = 0;
}

void sys_getrlimit(syscall_frame_t *frame) {
    int resource = (int)frame->rdi;
    rlimit_t *rlim = (rlimit_t *)frame->rsi;

    if (!rlim) { frame->rax = (uint64_t)-EINVAL; return; }

    rlimit_t lim;
    int ret = fill_rlimit(resource, &lim);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }

    write_vmm(current_task_ptr->ctx, (uint64_t)rlim, &lim, sizeof(lim));
    frame->rax = 0;
}

void sys_getrusage(syscall_frame_t *frame) {
    int who = (int)frame->rdi;
    struct rusage *usage = (struct rusage *)frame->rsi;

    if (!usage) { frame->rax = (uint64_t)-EINVAL; return; }
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) { frame->rax = (uint64_t)-EINVAL; return; }

    struct rusage ru = {0};
    if (who == RUSAGE_SELF) {
        uint64_t usec = hpet_elapsed_us();
        ru.ru_stime.tv_sec = (time_t)(usec / 1000000ULL);
        ru.ru_stime.tv_usec = (suseconds_t)(usec % 1000000ULL);
    }

    write_vmm(current_task_ptr->ctx, (uint64_t)usage, &ru, sizeof(ru));
    frame->rax = 0;
}

void sys_times(syscall_frame_t *frame) {
    tms_t *buf = (tms_t *)frame->rdi;
    uint64_t ticks = hpet_elapsed_us() / 10000ULL;

    if (buf) {
        tms_t t = {0};
        t.tms_stime = (clock_t)ticks;
        write_vmm(current_task_ptr->ctx, (uint64_t)buf, &t, sizeof(t));
    }

    frame->rax = (uint64_t)ticks;
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
        frame->rax = 0;
        return;
    }

    if (uid == current_task_ptr->uid || uid == current_task_ptr->euid) {
        current_task_ptr->euid = uid;
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
        frame->rax = 0;
        return;
    }

    if (gid == current_task_ptr->gid || gid == current_task_ptr->egid) {
        current_task_ptr->egid = gid;
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
        if (tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            target = &tasks[i];
            break;
        }
    }
    if (!target) { frame->rax = (uint64_t)-ESRCH; return; }

    // Target must be the caller or a child of the caller
    if (target->pid != current_task_ptr->pid &&
        target->ppid != current_task_ptr->pid) {
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

void sys_getsid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    if (pid == 0) {
        frame->rax = (uint64_t)current_task_ptr->sid;
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            // Only allowed to query tasks in same session
            if (tasks[i].sid != current_task_ptr->sid) {
                frame->rax = (uint64_t)-EPERM;
            } else {
                frame->rax = (uint64_t)tasks[i].sid;
            }
            return;
        }
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_seteuid(syscall_frame_t *frame) {
    uid_t euid = (uid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->euid = euid;
        frame->rax = 0;
        return;
    }

    if (euid == current_task_ptr->uid || euid == current_task_ptr->euid) {
        current_task_ptr->euid = euid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setegid(syscall_frame_t *frame) {
    gid_t egid = (gid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->egid = egid;
        frame->rax = 0;
        return;
    }

    if (egid == current_task_ptr->gid || egid == current_task_ptr->egid) {
        current_task_ptr->egid = egid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_utime(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));

    rootfs_file_t file = read_rootfs(abs_path);
    if (!file.data && !file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    frame->rax = 0;
}

void sys_arch_prctl(syscall_frame_t *frame) {
    int code = (int)frame->rdi;
    unsigned long addr = (unsigned long)frame->rsi;

    switch (code) {
        case ARCH_SET_FS:
            // Validate user-space address for FS base (TLS pointer)
            if (addr != 0 && !user_addr_ok(addr, 1)) {
                frame->rax = (uint64_t)-EPERM; return;
            }
            current_task_ptr->fs_base = addr;
            write_msr(MSR_FS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_FS:
            if (!user_ptr_ok((void*)addr, sizeof(uint64_t))) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->fs_base, sizeof(uint64_t));
            frame->rax = 0;
            return;
        case ARCH_SET_GS:
            if (addr != 0 && !user_addr_ok(addr, 1)) {
                frame->rax = (uint64_t)-EPERM; return;
            }
            current_task_ptr->gs_base = addr;
            write_msr(MSR_GS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_GS:
            if (!user_ptr_ok((void*)addr, sizeof(uint64_t))) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->gs_base, sizeof(uint64_t));
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

    if (!rlim) { frame->rax = (uint64_t)-EINVAL; return; }
    int ret = fill_rlimit(resource, &current);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }

    rlimit_t requested;
    read_vmm(current_task_ptr->ctx, &requested, (uint64_t)rlim, sizeof(requested));
    if (requested.rlim_cur != current.rlim_cur || requested.rlim_max != current.rlim_max) { frame->rax = (uint64_t)-EPERM; return; }

    frame->rax = 0;
}

void sys_settimeofday(syscall_frame_t *frame) {
    const struct timeval *tv = (const struct timeval *)frame->rdi;
    const struct timezone *tz = (const struct timezone *)frame->rsi;

    if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }

    if (tz) {
        struct timezone ktz;
        read_vmm(current_task_ptr->ctx, &ktz, (uint64_t)tz, sizeof(ktz));
        if (ktz.tz_minuteswest < -15 * 60 || ktz.tz_minuteswest > 15 * 60) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    if (!tv) { frame->rax = 0; return; }

    struct timeval ktv;
    read_vmm(current_task_ptr->ctx, &ktv, (uint64_t)tv, sizeof(ktv));

    if (ktv.tv_sec < 0 || ktv.tv_usec < 0 || ktv.tv_usec >= 1000000 ||
        ktv.tv_sec > (TIME_T_MAX / 1000000)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t desired_us = ((uint64_t)ktv.tv_sec * 1000000ULL) + (uint64_t)ktv.tv_usec;
    time_set_realtime_us(desired_us);

    frame->rax = 0;
}

void sys_mount(syscall_frame_t *frame) {
    const char *source = (const char *)frame->rdi;
    const char *target = (const char *)frame->rsi;
    const char *fstype = (const char *)frame->rdx;
    unsigned long mountflags = (unsigned long)frame->r10;
    const void *data = (const void *)frame->r8;

    (void)source; (void)mountflags; (void)data;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    if (!target || !fstype) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[64];
    char fstype_buf[32];
    if (copy_from_user(target_buf, target, sizeof(target_buf)) < 0 || copy_from_user(fstype_buf, fstype, sizeof(fstype_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    if (!*target_buf || !*fstype_buf) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    rootfs_file_t dir = read_rootfs(target_buf);
    if ((dir.mode & 0040000) == 0 && !dir.data) {
        frame->rax = (uint64_t)-ENOENT; return;
    }

    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);
    // Dedup: an identical (path, fstype) already registered (e.g. /proc was
    // pre-registered at boot and init mounts it again) is a no-op success,
    // not a duplicate row.
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].active &&
            strcmp(mounts[i].path, target_buf) == 0 &&
            strcmp(mounts[i].filesystemtype, fstype_buf) == 0) {
            spin_unlock_irqrestore(&vfs_lock, irq);
            frame->rax = 0;
            return;
        }
    }
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            strncpy(mounts[i].path, target_buf, 63); mounts[i].path[63] = '\0';
            strncpy(mounts[i].filesystemtype, fstype_buf, 31); mounts[i].filesystemtype[31] = '\0';
            mounts[i].active = true;
            spin_unlock_irqrestore(&vfs_lock, irq);
            frame->rax = 0;
            return;
        }
    }
    spin_unlock_irqrestore(&vfs_lock, irq);
    frame->rax = (uint64_t)-ENOMEM;
}

void sys_umount(syscall_frame_t *frame) {
    const char *target = (const char *)frame->rdi;
    (void)frame->rsi;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    if (!target) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[64];
    if (copy_from_user(target_buf, target, sizeof(target_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].path, target_buf) == 0) {
            mounts[i].active = false;
            spin_unlock_irqrestore(&vfs_lock, irq);
            frame->rax = 0;
            return;
        }
    }
    spin_unlock_irqrestore(&vfs_lock, irq);
    frame->rax = (uint64_t)-ENOENT;
}

void sys_sethostname(syscall_frame_t *frame) {
    const char *user_name = (const char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }
    if (!user_name || len == 0 || len >= HOSTNAME_MAX_LEN) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    if (!user_ptr_ok(user_name, len)) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    // Copy hostname from user-space to kernel buffer
    char name_buf[HOSTNAME_MAX_LEN];
    read_vmm(current_task_ptr->ctx, name_buf, (uint64_t)user_name, len);
    name_buf[len] = '\0';
    frame->rax = set_hostname(name_buf, len);
}

void sys_gethostname(syscall_frame_t *frame) {
    char *user_name = (char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    if (!user_name || len == 0) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    if (!user_addr_ok((uint64_t)user_name, len)) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    // get_hostname writes directly to a kernel buffer, then we copy to user
    char name_buf[HOSTNAME_MAX_LEN];
    int ret = get_hostname(name_buf, len < HOSTNAME_MAX_LEN ? len : HOSTNAME_MAX_LEN);
    if (ret == 0) {
        size_t copy_len = strlen(name_buf) + 1;
        if (copy_len > len) copy_len = len;
        write_vmm(current_task_ptr->ctx, (uint64_t)user_name, name_buf, copy_len);
    }
    frame->rax = (uint64_t)ret;
}

void sys_exit_group(syscall_frame_t *frame) {
    int status = (int)frame->rdi;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && 
            tasks[i].ctx == current_task_ptr->ctx &&
            &tasks[i] != current_task_ptr) {
            tasks[i].state = TASK_ZOMBIE;
            tasks[i].exit_status = status;
        }
    }

    exit_task(status);
}

void sys_openat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    uint32_t flags = (uint32_t)frame->rdx;
    mode_t mode = (mode_t)frame->r10;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

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
    if (is_mounted_under(abs_path, "devtmpfs", rel_path)) {
        if (rel_path[0] != '\0' && !devtmpfs_device_exists(rel_path)) {
            rootfs_file_t file = read_rootfs(abs_path);
            if (!(file.mode & 0040000)) {
                frame->rax = (uint64_t)-ENOENT;
                return;
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
    } else if (is_mounted_under(abs_path, "devpts", rel_path)) {
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
            rootfs_file_t file = read_rootfs(abs_path);
            if (!(file.mode & 0040000)) {
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

    // procfs: /proc, /proc/self, /proc/<pid>, /proc/<pid>/{maps,mounts,exe,...}
    {
        int pr = proc_open_common(abs_path, flags);
        if (pr != 1) { frame->rax = (uint64_t)pr; return; }
    }

    rootfs_file_t file = read_rootfs(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(abs_path, "", 0, mode | 0o100000, current_task_ptr->euid, current_task_ptr->egid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
        file = read_rootfs(abs_path);
    }

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);
    if (!can_access_rootfs(&file, want_read, want_write, 0)) { frame->rax = (uint64_t)-EACCES; return; }

    int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_FILE, flags);
    frame->rax = (uint64_t)fd;
}

void sys_unlinkat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    int flags = (int)frame->rdx;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path_buf, abs_path, sizeof(abs_path));

    if (res < 0) { frame->rax = (uint64_t)res; return; }

    // Don't follow the final component: unlinkat must remove the link itself.
    rootfs_file_t file = stat_rootfs_nofollow(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    if (flags & AT_REMOVEDIR) {
        int ret = rmdir_rootfs(abs_path);
        frame->rax = ret < 0 ? (uint64_t)ret : 0;
    } else {
        if ((file.mode & 0xF000) == 0x4000) { frame->rax = (uint64_t)-EISDIR; return; }
        int ret = delete_rootfs(abs_path);
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
    if (copy_from_user(target_buf, target, sizeof(target_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(newdirfd, path_buf, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    // Existence check on the link path itself, not a resolved target.
    rootfs_file_t existing = stat_rootfs_nofollow(abs_path);
    if (existing.mode) { frame->rax = (uint64_t)-EEXIST; return; }

    int ret = symlink_rootfs(target_buf, abs_path, current_task_ptr->euid, current_task_ptr->egid);

    frame->rax = ret < 0 ? (uint64_t)ret : 0;
}

void sys_fchmodat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    mode_t mode = (mode_t)frame->rdx;
    int flags = (int)frame->r10;

    (void)flags;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path_buf, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    // Permission check: only owner or root can chmod
    rootfs_file_t file = read_rootfs(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    frame->rax = (uint64_t)chmod_rootfs(abs_path, mode & 0777);
}

void sys_utimensat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *user_path = (const char *)frame->rsi;
    // frame->rdx = times[2] pointer (ignored — we don't apply it)
    // frame->r10 = flags

    if (dirfd != AT_FDCWD) {
        // Relative-to-dirfd is not supported; only AT_FDCWD/absolute paths.
        frame->rax = (uint64_t)-ENOTSUP; return;
    }
    if (!user_path) { frame->rax = (uint64_t)-EFAULT; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    char abs_path[256];
    build_abs_path(path_buf, abs_path, sizeof(abs_path));

    rootfs_file_t file = read_rootfs(abs_path);
    if (!file.data && !file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    frame->rax = 0;
}

void sys_futex(syscall_frame_t *frame) {
    uint32_t *uaddr = (uint32_t *)frame->rdi;
    int op = (int)frame->rsi;
    uint32_t val = (uint32_t)frame->rdx;
    struct timespec *timeout_ptr = (struct timespec *)frame->r10;
    uint32_t *uaddr2 = (uint32_t *)frame->r8;
    uint32_t val3 = (uint32_t)frame->r9;

    int cmd = op & FUTEX_CMD_MASK;

    uint64_t phys = futex_resolve_key(uaddr, frame);
    if (!phys) return;

    switch (cmd) {

    case FUTEX_WAIT: {
        futex_wait(frame, phys, val, timeout_ptr, FUTEX_BITSET_MATCH_ANY, false);
        return;
    }

    case FUTEX_WAIT_BITSET: {
        if (val3 == 0) { frame->rax = (uint64_t)-EINVAL; return; }

        futex_wait(frame, phys, val, timeout_ptr, val3, true);
        return;
    }

    case FUTEX_WAKE: {
        if (val3 == 0) val3 = FUTEX_BITSET_MATCH_ANY;
        int woken = futex_wake(phys, val, FUTEX_BITSET_MATCH_ANY);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_WAKE_BITSET: {
        if (val3 == 0) { frame->rax = (uint64_t)-EINVAL; return; }
        int woken = futex_wake(phys, val, val3);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_REQUEUE: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_ptr_ok(uaddr2, sizeof(uint32_t))) {
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
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx].state == TASK_STOPPED)
                    tasks[idx].state = TASK_READY;
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

        if (!uaddr2 || !user_ptr_ok(uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        uint32_t cur_val = 0;
        read_vmm(current_task_ptr->ctx, &cur_val, (uint64_t)uaddr, sizeof(uint32_t));
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
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx].state == TASK_STOPPED)
                    tasks[idx].state = TASK_READY;
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

        if (!uaddr2 || !user_ptr_ok(uaddr2, sizeof(uint32_t))) {
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
        read_vmm(current_task_ptr->ctx, &oldval, (uint64_t)uaddr2, sizeof(uint32_t));

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
        write_vmm(current_task_ptr->ctx, (uint64_t)uaddr2, &newval, sizeof(uint32_t));

        int woken = 0;
        for (int i = 0; i < MAX_FUTEX_WAITERS && (uint32_t)woken < val; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;
            futex_waiters[i].state = FW_WOKEN;
            int idx = futex_waiters[i].task_idx;
            if (idx >= 0 && idx < MAX_TASKS && tasks[idx].state == TASK_STOPPED)
                tasks[idx].state = TASK_READY;
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
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx].state == TASK_STOPPED)
                    tasks[idx].state = TASK_READY;
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
    if (!user_ptr_ok(optval, sizeof(int)) || !user_ptr_ok(optlen, sizeof(uint32_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (optname == SO_ERROR) val = get_unix_socket_error((unix_handle_t *)entry->handle);
    else if (optname == SO_TYPE) val = get_unix_socket_type((unix_handle_t *)entry->handle);
    else { frame->rax = (uint64_t)-ENOPROTOOPT; return; }
    write_vmm(current_task_ptr->ctx, (uint64_t)optval, &val, sizeof(int));
    uint32_t len = sizeof(int);
    write_vmm(current_task_ptr->ctx, (uint64_t)optlen, &len, sizeof(uint32_t));
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
        default:
            frame->rax = (uint64_t)-ENOPROTOOPT;
            return;
    }
}

void sys_getrandom(syscall_frame_t *frame) {
    uint8_t *buf = (uint8_t *)frame->rdi;
    uint64_t buflen = frame->rsi;
    unsigned int flags = (unsigned int)frame->rdx;

    if (buflen > 256) { frame->rax = (uint64_t)-EINVAL; return; }
    if (buflen == 0) { frame->rax = 0; return; }
    if (!buf) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_ptr_ok(buf, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

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
        write_vmm(current_task_ptr->ctx, (uint64_t)buf + copied, &rand_val, to_copy);
        copied += to_copy;
    }
    frame->rax = buflen;
}

void sys_poll(syscall_frame_t *frame) {
    struct pollfd *user_fds = (struct pollfd *)frame->rdi;
    uint64_t nfds = (uint64_t)frame->rsi;
    int timeout = (int)frame->rdx;
    if (nfds > 1024) { frame->rax = (uint64_t)-EINVAL; return; }
    if (nfds > 0 && !user_ptr_ok(user_fds, nfds * sizeof(struct pollfd))) {
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
                    int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 0; \
                    tty_t *t = get_tty(tty_idx); \
                    if (t && get_tty_ring_count(&t->input) > 0) k_fds[i].revents |= POLLIN; \
                } else if (entry->type == FD_DEV) { \
                    char rel[256]; \
                    int tty_idx = -1; \
                    if (is_mounted_under(entry->path, "devtmpfs", rel)) { \
                        if (strncmp(rel, "tty", 3) == 0 || strcmp(rel, "console") == 0) { \
                            tty_idx = (strlen(rel) > 3 && rel[3] >= '0' && rel[3] <= '7') ? rel[3] - '0' : 0; \
                        } \
                    } else if (is_mounted_under(entry->path, "devpts", rel)) { \
                        tty_idx = current_task_ptr->ctty_idx; \
                    } \
                    if (tty_idx >= 0) { \
                        tty_t *t = get_tty(tty_idx); \
                        if (t && get_tty_ring_count(&t->input) > 0) k_fds[i].revents |= POLLIN; \
                    } else { \
                        k_fds[i].revents |= POLLIN; \
                    } \
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
        uint64_t start_time = hpet_elapsed_us();
        uint64_t total_us = (timeout > 0) ? (uint64_t)timeout * 1000ULL : UINT64_MAX;
        while (1) {
            if (signal_pending()) {
                if (nfds > 0) free(k_fds);
                frame->rax = (uint64_t)-EINTR;
                return;
            }
            EVAL_FDS(events);
            if (events > 0) break;
            if (timeout > 0 && hpet_elapsed_us() - start_time >= total_us) break;
            __asm__ volatile("int $32");
        }
    }

    #undef EVAL_FDS

    if (nfds > 0) {
        copy_to_user((void*)user_fds, k_fds, nfds * sizeof(struct pollfd));
        free(k_fds);
    }
    frame->rax = (uint64_t)events;
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
        if (!user_ptr_ok(oldset, 8)) { frame->rax = (uint64_t)-EFAULT; return; }
        write_vmm(current_task_ptr->ctx, (uint64_t)oldset, &current_task_ptr->blocked_signals, 8);
    }

    if (set) {
        uint64_t new_set;
        if (copy_from_user(&new_set, set, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

        new_set &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1))); // SIGKILL and SIGSTOP

        if (how == 0) { // SIG_BLOCK
            current_task_ptr->blocked_signals |= new_set;
        } else if (how == 1) { // SIG_UNBLOCK
            current_task_ptr->blocked_signals &= ~new_set;
        } else if (how == 2) { // SIG_SETMASK
            current_task_ptr->blocked_signals = new_set;
        } else {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
    }

    frame->rax = 0;
}

void sys_set_tid_address(syscall_frame_t *frame) {
    int *tidptr = (int *)frame->rdi;
    current_task_ptr->clear_child_tid = tidptr;
    frame->rax = current_task_ptr->pid;
}
