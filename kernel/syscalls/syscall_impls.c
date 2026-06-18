#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/sys/futex.h>
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
#include <freestanding/errno.h>
#include <freestanding/asm/prctl.h>
#include <freestanding/sys/ioctl.h>
#include <freestanding/sys/socket.h>
#include <freestanding/sys/mman.h>
#include <freestanding/sys/fb.h>
#include <freestanding/sys/stat.h>
#include <freestanding/sys/resource.h>
#include <freestanding/sys/reboot.h>
#include <freestanding/sys/random.h>
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
#include <main/uname.h>
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
#include <mm/mm.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>

/* Tried to fucking modularize this...
   Didn't go well...
   To who is reading this:
     - Please don't try to modularize this, it's too late...just keep adding on...
*/

// Should be never exposed to other files
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
    if (!user_arr || !user_ptr_ok(user_arr, sizeof(char *))) return -EFAULT;

    char **k_arr = malloc((max_elements + 1) * sizeof(char *));
    if (!k_arr) return -ENOMEM;

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
        current_task_ptr->state = TASK_STOPPED;
        spin_unlock_irqrestore(&futex_lock, irq_flags);
    }

    int wake_state = futex_waiters[slot].state;
    futex_waiters[slot].state = FW_FREE;
    spin_unlock_irqrestore(&futex_lock, irq_flags);

    if (wake_state == FW_TIMED_OUT) {
        frame->rax = (uint64_t)-ETIMEDOUT;
    } else if (current_task_ptr->pending_signals) {

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

void check_signals(syscall_frame_t *frame) {
    if (!current_task_ptr) return;
    if (current_task_ptr->pending_signals == 0) return;

    for (int i = 1; i < 32; i++) {
        if (current_task_ptr->pending_signals & (1ULL << i)) {
            uint64_t *sa = &current_task_ptr->sigactions[i * 4];
            uint64_t handler = sa[0];
            uint64_t flags = sa[2];
            uint64_t restorer = sa[3];

            if (handler == 0 ) {
                if (i != 17 ) {
                    current_task_ptr->pending_signals = 0;
                    exit_task(128 + i);
                }
                current_task_ptr->pending_signals &= ~(1ULL << i);
                continue;
            } else if (handler == 1 ) {
                current_task_ptr->pending_signals &= ~(1ULL << i);
                continue;
            }

            current_task_ptr->pending_signals &= ~(1ULL << i);

            uint64_t user_rsp = frame->r12 - 128; // red zone
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
            frame->r12 = user_rsp;

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

void sys_read(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint8_t *buf = (uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    // Validate user buffer
    if (count > 0 && !user_ptr_ok(buf, count)) { frame->rax = (uint64_t)-EFAULT; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (fd == 0) {
        // Use per-task stdin buffer
        char *sbuf = current_task_ptr->stdin_buf;
        int *sbuf_len = &current_task_ptr->stdin_buf_len;
        int *sbuf_pos = &current_task_ptr->stdin_buf_pos;
        uint64_t irq;
        spin_lock_irqsave(&stdin_lock, &irq);
        if (*sbuf_pos >= *sbuf_len) {
            *sbuf_len = 0;
            *sbuf_pos = 0;
            spin_unlock_irqrestore(&stdin_lock, irq);

            puts("\033[s");
            while (1) {
                current_task_ptr->state = TASK_READY;
                spin_unlock(&sched_lock);
                char c = getc();
                spin_lock(&sched_lock);

                current_task_ptr->state = TASK_RUNNING;

                spin_lock_irqsave(&stdin_lock, &irq);
                if (c == '\b' || c == 127) {
                    if (*sbuf_len > 0) { (*sbuf_len)--; puts("\b \b"); }
                    spin_unlock_irqrestore(&stdin_lock, irq);
                    continue;
                }

                if (c == '\n') {
                    putc(c);
                    if (*sbuf_len < TASK_STDIN_BUF_SIZE) {
                        sbuf[(*sbuf_len)++] = c;
                    }
                    spin_unlock_irqrestore(&stdin_lock, irq);
                    break;
                }

                if (*sbuf_len < TASK_STDIN_BUF_SIZE - 1) { putc(c); sbuf[(*sbuf_len)++] = c; }

                spin_unlock_irqrestore(&stdin_lock, irq);
            }
            spin_lock_irqsave(&stdin_lock, &irq);
        }

        uint64_t avail = (uint64_t)(*sbuf_len - *sbuf_pos);
        uint64_t to_copy = (count < avail) ? count : avail;
        if (to_copy > 0) {
            char tmp[512];
            uint64_t total_copied = 0;
            while (total_copied < to_copy) {
                uint64_t chunk = to_copy - total_copied;
                if (chunk > sizeof(tmp)) chunk = sizeof(tmp);

                memcpy(tmp, sbuf + *sbuf_pos, chunk);
                *sbuf_pos += (int)chunk;

                spin_unlock_irqrestore(&stdin_lock, irq);
                write_vmm(current_task_ptr->ctx, (uint64_t)buf + total_copied, tmp, chunk);
                total_copied += chunk;
                if (total_copied < to_copy) spin_lock_irqsave(&stdin_lock, &irq);
            }
        } else {
            spin_unlock_irqrestore(&stdin_lock, irq);
        }

        frame->rax = to_copy;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        uint64_t res;
        if (is_mounted_under(entry->path, "devtmpfs", rel)) {
            if (count == 0 || count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
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
                putc(kbuf[i]);
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

    rootfs_file_t file = read_rootfs(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(abs_path, "", 0, mode, current_task_ptr->euid, current_task_ptr->egid);
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

    rootfs_file_t file = read_rootfs(path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    struct stat kst = {0};
    kst.st_mode = file.mode;
    kst.st_uid = file.uid;
    kst.st_gid = file.gid;
    kst.st_size = file.size;

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

    if (entry->type == FD_FILE) {
        rootfs_file_t file = read_rootfs(entry->path);
        struct stat kst = {0};
        kst.st_mode = file.mode;
        kst.st_uid = file.uid;
        kst.st_gid = file.gid;
        kst.st_size = file.size;
        write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat));
        frame->rax = 0;
        return;
    }
    frame->rax = (uint64_t)-EBADF;
}

void sys_lseek(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int64_t offset = (int64_t)frame->rsi;
    int whence = (int)frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry || !entry->open) { frame->rax = -EBADF; return; }
    if (entry->type == FD_STREAM || entry->type == FD_PIPE || entry->type == FD_SOCKET) { frame->rax = -ESPIPE; return; }

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
            rootfs_file_t file = read_rootfs(entry->path);
            if (!file.data) { frame->rax = -ENOENT; return; }
            int64_t new_off = (int64_t)file.size + offset;
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

    // Guard against integer overflow in page-count calculation
    if (length > USER_ADDR_MAX) { frame->rax = (uint64_t)-EINVAL; return; }

    // Validate addr if MAP_FIXED or hint provided
    if (addr != 0 && !user_addr_ok(addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }

    // Reject W+X mappings (W^X policy)
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) { frame->rax = (uint64_t)-EACCES; return; }

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
        ptr = vmap_user_at(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE, vmm_flags);
        if (!ptr) ptr = vmalloc_ex(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    } else {
        ptr = vmalloc_ex(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    }

    if (!ptr) { frame->rax = (uint64_t)-ENOMEM; return; }

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
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) { frame->rax = (uint64_t)-EACCES; return; }

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
    uint64_t new_brk = (addr + 0xFFF) & ~0xFFFULL;
    uint64_t old_brk = (current_task_ptr->brk + 0xFFF) & ~0xFFFULL;

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
    uint64_t user_rsp = frame->r12;
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
                if (strncmp(rel, "tty", 3) == 0 || strncmp(rel, "pts/", 4) == 0 || strcmp(rel, "console") == 0)
                    is_tty = 1;
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
        termios_t t = {0};
        if (entry && entry->type == FD_DEV) {
            char rel[256];
            if (is_mounted_under(entry->path, "devtmpfs", rel)) {
                if (strncmp(rel, "tty", 3) == 0) {
                     int idx = rel[3] - '0';
                     if (idx >= 0 && idx < NUM_TTYS) { t = get_tty(idx)->termios; } } else if (strncmp(rel, "pts/", 4) == 0) {
                    t.c_iflag = 0x0500;
                    t.c_oflag = 0x0005;
                    t.c_cflag = 0x04BF;
                    t.c_lflag = 0x8A3B;
                    t.c_cc[4] = 1;
                }
            } else if (is_mounted_under(entry->path, "devpts", rel)) {
                t.c_iflag = 0x0500;
                t.c_oflag = 0x0005;
                t.c_cflag = 0x04BF;
                t.c_lflag = 0x8A3B;
                t.c_cc[4] = 1;
            }
        }
        write_vmm(current_task_ptr->ctx, argp, &t, sizeof(t));
        frame->rax = 0;
        return;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (entry && entry->type == FD_DEV) {
            char rel[256];
            if (is_mounted_under(entry->path, "devtmpfs", rel)) {
                if (strncmp(rel, "tty", 3) == 0) {
                     int idx = rel[3] - '0';
                     if (idx >= 0 && idx < NUM_TTYS) { read_vmm(current_task_ptr->ctx, &get_tty(idx)->termios, argp, sizeof(termios_t)); }
                }
            }
        }
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
                } else if (strncmp(rel, "pts/", 4) == 0) {
                     int idx = 0;
                     const char *p = rel + 4;
                     while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
                     if (idx >= 0 && idx < NUM_PTYS) {
                         if (argp == 0 || argp == 2) {
                             get_pty(idx)->m2s.head = get_pty(idx)->m2s.tail = 0;
                         }
                     }
                }
            } else if (is_mounted_under(entry->path, "devpts", rel)) {
                 int idx = 0;
                 const char *p = rel;
                 while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
                 if (idx >= 0 && idx < NUM_PTYS) {
                     if (argp == 0 || argp == 2) {
                         get_pty(idx)->m2s.head = get_pty(idx)->m2s.tail = 0;
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

    case TIOCGPGRP: {
        // Return our own PID as the foreground process group
        pid_t pgrp = current_task_ptr->pid;
        write_vmm(current_task_ptr->ctx, argp, &pgrp, sizeof(pid_t));
        frame->rax = 0;
        return;
    }
    case TIOCSPGRP:
        // Accept but ignore foreground pgrp sets (no job control yet)
        frame->rax = 0;
        return;

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
        // Exclusive mode — no-op
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
    sleep_us(total_us);

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
    int how = (int)frame->rdi;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    switch (how) {
        case RB_REBOOT:
            reboot();
            __builtin_unreachable();
        case RB_POWEROFF:
            poweroff();
            __builtin_unreachable();
        case RB_HALT:
            halt();
            __builtin_unreachable();
        default:
            // We'll return EINVAL later, just break
            break;
    }

    frame->rax = -EINVAL;
}

void sys_fork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { frame->rax = (uint64_t)-EAGAIN; return; }

    frame->rax = (uint64_t)child_pid;
}

void sys_execve(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    char **user_argv = (char **)frame->rsi;
    char **user_envp = (char **)frame->rdx;

    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    rootfs_file_t file = read_rootfs(path_buf);
    if (file.mode && !can_access_rootfs(&file, 0, 0, 1)) { frame->rax = (uint64_t)-EPERM; return; }

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
    }

    free_strarray(argv_ptrs, argc);
    free_strarray(envp_ptrs, envc);

    frame->rax = (uint64_t)res;
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
            if (!tasks[i].state || tasks[i].ppid != current_task_ptr->pid)
                continue;

            // If user asked for a specific PID, ignore others
            if (pid != -1 && tasks[i].pid != pid)
                continue;

            found_child = 1;

            if (tasks[i].state == TASK_ZOMBIE) {
                // Reap the zombie
                if (wstatus) { int status = tasks[i].exit_status << 8; write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int)); }

                if (rusage) { struct rusage ru = {0}; write_vmm(current_task_ptr->ctx, (uint64_t)rusage, &ru, sizeof(struct rusage)); }

                pid_t ret = tasks[i].pid;
                tasks[i].state = TASK_DEAD; // Clean up the task slot
                frame->rax = (uint64_t)ret;
                return; // Successfully reaped!
            }
        }

        // If we found no children at all, we can't wait
        if (!found_child) { frame->rax = -ECHILD; return; }

        // WNOHANG: return 0 if no child has exited
        if (options & WNOHANG) { frame->rax = 0; return; }

        // We need to yield to let the child run. But sched_lock is held
        // by syscall_handler.S during all syscalls, and the timer ISR
        // skips context switching when sched_lock != 0. So we must
        // temporarily drop the lock before yielding.
        current_task_ptr->state = TASK_READY;
        spin_unlock(&sched_lock);
        __asm__ volatile("int $32");
        spin_lock(&sched_lock);
    }
}

void sys_kill(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;

    // pid 0 or negative would require process-group semantics; reject for now
    if (pid <= 0) { frame->rax = (uint64_t)-EINVAL; return; }

    // Accept signal 0 (existence check) and standard POSIX signals 1-31
    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            // Permission check: root or same UID
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i].uid) { frame->rax = (uint64_t)-EPERM; return; }

            if (pid == 1 && sig != 0) { frame->rax = (uint64_t)-EPERM; return; }

            if (sig == 0) { frame->rax = 0; return; }

            switch (sig) {
                case SIGSTOP:
                case SIGTSTP:
                    if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_READY) {
                        tasks[i].state = TASK_STOPPED;
                        if (pid == current_task_ptr->pid) { __asm__ volatile("int $32"); } }
                    break;
                case SIGCONT:
                    if (tasks[i].state == TASK_STOPPED) {
                        tasks[i].state = TASK_READY;
                    }
                    break;
                default:
                    if (sig == 9 ) {
                        if (pid == current_task_ptr->pid) { exit_task(128 + sig); }
                        tasks[i].state = TASK_ZOMBIE;
                        tasks[i].exit_status = 128 + sig;
                        for (int j = 1; j < MAX_TASKS; j++) {
                            if (tasks[j].state != TASK_DEAD && tasks[j].ppid == pid) {
                                if (tasks[j].state == TASK_ZOMBIE) { tasks[j].state = TASK_DEAD; } else { tasks[j].ppid = 1; }
                            }
                        }
                        // Close file descriptors
                        for (int j = 0; j < FD_MAX; j++) {
                            if (tasks[i].fd_table.entries[j].open) { free_fd(&tasks[i].fd_table, j); }
                        }
                    } else {
                        tasks[i].pending_signals |= (1ULL << sig);
                    }
                    break;
            }

            frame->rax = 0;
            return;
        }
    }

    frame->rax = (uint64_t)-ESRCH;
}

void sys_uname(syscall_frame_t *frame) {
    uint64_t bufp = frame->rdi;

    if (!bufp) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    utsname_t info = uname_info;
    get_hostname(info.nodename, sizeof(info.nodename));

    write_vmm(current_task_ptr->ctx, bufp, &info, sizeof(info));
    frame->rax = 0;
}

void sys_flock(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int operation = (int)frame->rsi;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (entry->type != FD_FILE) { frame->rax = (uint64_t)-EBADF; return; }

    int op = operation & ~LOCK_NB;
    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN) { frame->rax = (uint64_t)-EINVAL; return; }

    extern void sleep(uint64_t ms);

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
    // Reject zero-length buffer or invalid pointer
    if (buflen == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_addr_ok(bufp, buflen)) { frame->rax = (uint64_t)-EFAULT; return; }

    char dir_norm[256];
    strncpy(dir_norm, entry->path, sizeof(dir_norm) - 1);
    dir_norm[sizeof(dir_norm) - 1] = '\0';
    size_t dlen = strlen(dir_norm);
    if (dlen > 1 && dir_norm[dlen - 1] == '/') dir_norm[--dlen] = '\0';

    uint64_t written = 0;

    // Check if this directory is a devtmpfs mount point
    char rel[256];
    if (is_mounted_under(entry->path, "devtmpfs", rel)) {
        int index = (int)entry->offset;
        // Enumerate devtmpfs devices
        while (written + sizeof(dirent_t) <= buflen) {
            const char *devname = devtmpfs_get_device_name(index);
            if (!devname) break;

            dirent_t d;
            memset(&d, 0, sizeof(d));
            d.d_ino = (uint64_t)(index + 1);
            d.d_off = (int64_t)(index + 1);
            d.d_reclen = (uint16_t)sizeof(dirent_t);
            d.d_type = DT_REG;
            strncpy(d.d_name, devname, 255);
            d.d_name[255] = '\0';

            write_vmm(current_task_ptr->ctx, bufp + written, &d, sizeof(d));
            written += sizeof(dirent_t);
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    if (is_mounted_under(entry->path, "devpts", rel)) {
        int index = (int)entry->offset;
        // Enumerate devpts devices
        while (written + sizeof(dirent_t) <= buflen) {
            const char *devname = devpts_get_device_name(index);
            if (!devname) break;

            dirent_t d;
            memset(&d, 0, sizeof(d));
            d.d_ino = (uint64_t)(index + 1);
            d.d_off = (int64_t)(index + 1);
            d.d_reclen = (uint16_t)sizeof(dirent_t);
            d.d_type = DT_CHR;
            strncpy(d.d_name, devname, 255);
            d.d_name[255] = '\0';

            write_vmm(current_task_ptr->ctx, bufp + written, &d, sizeof(d));
            written += sizeof(dirent_t);
            index++;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    // Normal rootfs enumeration
    int index = (int)entry->offset;
    while (written + sizeof(dirent_t) <= buflen) {
        directory_entry_t de;
        if (get_rootfs_entry(index, &de) != 0) break;

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

        char prefix[258];
        strncpy(prefix, dir_norm, sizeof(prefix) - 2);
        if (strcmp(dir_norm, "/") != 0) strcat(prefix, "/");
        size_t prefix_len = strlen(prefix);

        if (strncmp(abs_entry, prefix, prefix_len) != 0) { index++; continue; }

        const char *child = abs_entry + prefix_len;
        if (!*child || strchr(child, '/')) { index++; continue; }

        dirent_t d;
        memset(&d, 0, sizeof(d));
        d.d_ino = (uint64_t)(index + 1);
        d.d_off = (int64_t)(index + 1);
        d.d_reclen = (uint16_t)sizeof(dirent_t);
        d.d_type = (de.type == FT_DIRECTORY) ? DT_DIR :
                   (de.type == FT_SYMLINK) ? DT_LNK : DT_REG;
        strncpy(d.d_name, child, 255);
        d.d_name[255] = '\0';

        write_vmm(current_task_ptr->ctx, bufp + written, &d, sizeof(d));
        written += sizeof(dirent_t);
        index++;
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

    write_vmm(current_task_ptr->ctx, bufp, current_task_ptr->cwd, cwd_len);
    frame->rax = bufp;
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

    rootfs_file_t dir = read_rootfs(abs_path);
    if (!dir.data && !dir.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if ((dir.mode & 0040000) == 0 && strcmp(abs_path, "/") != 0) {
        frame->rax = (uint64_t)-ENOTDIR; return;
    }

    strncpy(current_task_ptr->cwd, abs_path, 255);
    current_task_ptr->cwd[255] = '\0';
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

void sys_unlink(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char path_buf[256];
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    get_absolute_path(path_buf, abs_path, sizeof(abs_path));

    rootfs_file_t file = stat_rootfs(abs_path);
    if (!file.mode) { frame->rax = (uint64_t)-ENOENT; return; }

    if (current_task_ptr->euid != 0 && current_task_ptr->euid != file.uid) { frame->rax = (uint64_t)-EPERM; return; }

    int ret = delete_rootfs(abs_path);
    if (ret < 0) { frame->rax = (uint64_t)-ENOENT; return; }

    frame->rax = 0;
}

void sys_symlink(syscall_frame_t *frame) {
    const char *user_target = (const char *)frame->rdi;
    const char *user_linkpath = (const char *)frame->rsi;

    if (!user_target || !user_linkpath) { frame->rax = (uint64_t)-EINVAL; return; }

    char target[256], linkpath[256];
    if (copy_from_user(target, user_target, sizeof(target)) < 0 || copy_from_user(linkpath, user_linkpath, sizeof(linkpath)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_linkpath[256];
    get_absolute_path(linkpath, abs_linkpath, sizeof(abs_linkpath));

    frame->rax = (uint64_t)symlink_rootfs(target, abs_linkpath, current_task_ptr->euid, current_task_ptr->egid);
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
    int ret = chmod_rootfs(abs_path, mode);

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

    frame->rax = (uint64_t)chmod_rootfs(entry->path, mode);
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

void sys_getppid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->ppid;
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
    const char *user_target = (const char *)frame->rsi;
    const char *user_fstype = (const char *)frame->rdx;
    unsigned long mountflags = (unsigned long)frame->r10;
    const void *data = (const void *)frame->r8;

    (void)source; (void)mountflags; (void)data;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    if (!user_target || !user_fstype) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    char target_buf[64];
    char fstype_buf[32];
    if (copy_from_user(target_buf, user_target, sizeof(target_buf)) < 0 || copy_from_user(fstype_buf, user_fstype, sizeof(fstype_buf)) < 0) {
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
    const char *user_target = (const char *)frame->rdi;
    (void)frame->rsi;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

    if (!user_target) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[64];
    if (copy_from_user(target_buf, user_target, sizeof(target_buf)) < 0) {
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

    rootfs_file_t file = read_rootfs(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(abs_path, "", 0, mode, current_task_ptr->euid, current_task_ptr->egid);
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

    rootfs_file_t file = stat_rootfs(abs_path);
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
    const char *user_target = (const char *)frame->rdi;
    int newdirfd = (int)frame->rsi;
    const char *user_path = (const char *)frame->rdx;

    if (!user_target || !user_path) { frame->rax = (uint64_t)-EINVAL; return; }

    char target_buf[256];
    char path_buf[256];
    if (copy_from_user(target_buf, user_target, sizeof(target_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (copy_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    char abs_path[256];
    int res = build_abs_path_at(newdirfd, path_buf, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    rootfs_file_t existing = stat_rootfs(abs_path);
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

    frame->rax = (uint64_t)chmod_rootfs(abs_path, mode);
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
