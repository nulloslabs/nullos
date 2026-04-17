#include <syscalls/syscalls.h>
#include <main/fd.h>
#include <main/rootfs.h>
#include <main/scheduler.h>
#include <main/string.h>
#include <main/errno.h>
#include <io/terminal.h>
#include <io/keyboard.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <freestanding/fcntl.h>
#include <main/devfs.h>
#include <main/spinlock.h>
#include <main/elf.h>
#include <main/halt.h>
#include <main/hostname.h>

extern volatile int sched_lock;

static char stdin_buf[256];
static int stdin_buf_len = 0;
static int stdin_buf_pos = 0;

#define MAX_MOUNTS 16
typedef struct {
    char path[64];
    char filesystemtype[32];
    bool active;
} mount_t;

static mount_t mounts[MAX_MOUNTS];
static spinlock_t vfs_lock = SPINLOCK_INIT;

#define TIOCGWINSZ 0x5413

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} __attribute__((packed)) dirent_t;

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

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

    if (depth == 0) {
        strcpy(path, "/");
        return;
    }

    char out[256];
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strcat(out, "/");
        strncat(out, parts[i], sizeof(out) - strlen(out) - 1);
    }
    strncpy(path, out, 255);
    path[255] = '\0';
}

static void build_abs_path(const char *path, char *out, size_t out_size) {
    if (path[0] == '/') {
        strncpy(out, path, out_size - 1);
    } else {
        strncpy(out, current_task_ptr->cwd, out_size - 1);
        if (strcmp(out, "/") != 0)
            strncat(out, "/", out_size - strlen(out) - 1);
        strncat(out, path, out_size - strlen(out) - 1);
    }
    out[out_size - 1] = '\0';
    normalize_path_str(out);
}

static int copy_user_strarray(char **user_arr, char **out, int max_count) {
    int count = 0;
    if (!user_arr) return 0;
    while (count < max_count) {
        char *u_ptr;
        read_vmm(current_task_ptr->ctx, &u_ptr,
                 (uint64_t)&user_arr[count], sizeof(char *));
        if (!u_ptr) break;
        char *k_str = malloc(256);
        if (!k_str) break;
        read_vmm(current_task_ptr->ctx, k_str, (uint64_t)u_ptr, 255);
        k_str[255] = '\0';
        out[count++] = k_str;
    }
    out[count] = NULL;
    return count;
}

static void free_strarray(char **arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i]);
}

void sys_exit(syscall_frame_t *frame) {
    int status = (int)frame->rdi;
    exit_task(status);
}

void sys_open(syscall_frame_t *frame) {
    const char *path = (const char *)frame->rdi;
    uint32_t flags = (uint32_t)frame->rsi;
    mode_t mode = (mode_t)frame->rdx;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    char rel_path[256];
    if (is_mounted_under(path, "devfs", rel_path)) {
        if (!devfs_device_exists(rel_path)) { frame->rax = (uint64_t)-ENOENT; return; }
        int fd = alloc_fd(&current_task_ptr->fd_table, path, FD_DEV, flags);
        frame->rax = (uint64_t)fd;
        return;
    }

    rootfs_file_t file = read_rootfs(path);

    if (!file.data && !(file.mode & 0040000) && !(flags & O_CREAT)) {
        frame->rax = (uint64_t)-ENOENT;
        return;
    }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(path, "", 0, mode);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
    }

    int fd = alloc_fd(&current_task_ptr->fd_table, path, FD_FILE, flags);
    frame->rax = (uint64_t)fd;
}

void sys_close(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int res = free_fd(&current_task_ptr->fd_table, fd);
    frame->rax = (res < 0) ? (uint64_t)res : 0;
}

void sys_read(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint8_t *buf = (uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (fd == 0) {
        uint64_t irq;
        spin_lock_irqsave(&vfs_lock, &irq);

        if (stdin_buf_pos >= stdin_buf_len) {
            stdin_buf_len = 0;
            stdin_buf_pos = 0;
            reset_term_line_start();
            while (stdin_buf_len < (int)sizeof(stdin_buf) - 1) {
                char c = getc();
                if (c == '\b' || c == 127) {
                    if (stdin_buf_len > 0) { stdin_buf_len--; puts("\b \b"); }
                    continue;
                }
                putc(c);
                stdin_buf[stdin_buf_len++] = c;
                if (c == '\n') break;
            }
        }

        uint64_t copied = 0;
        while (copied < count && stdin_buf_pos < stdin_buf_len)
            buf[copied++] = (uint8_t)stdin_buf[stdin_buf_pos++];

        spin_unlock_irqrestore(&vfs_lock, irq);
        frame->rax = copied;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        if (!is_mounted_under(entry->path, "devfs", rel)) {
            frame->rax = (uint64_t)-ENODEV; return;
        }
        uint64_t res = read_devfs(rel, buf, count, entry->offset);
        if ((int64_t)res >= 0) entry->offset += res;
        frame->rax = res;
        return;
    }

    rootfs_file_t file = read_rootfs(entry->path);
    if (!file.data || entry->offset >= file.size) { frame->rax = 0; return; }

    uint64_t avail = file.size - entry->offset;
    uint64_t to_read = (count < avail) ? count : avail;
    memcpy(buf, (uint8_t *)file.data + entry->offset, to_read);
    entry->offset += to_read;
    frame->rax = to_read;
}

void sys_write(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const uint8_t *buf = (const uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    if (!buf) { frame->rax = (uint64_t)-EINVAL; return; }

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (fd == 1 || fd == 2) {
        char kbuf[256];
        uint64_t processed = 0;
        while (processed < count) {
            uint64_t chunk = count - processed;
            if (chunk > 255) chunk = 255;
            for (uint64_t i = 0; i < chunk; i++) {
                read_vmm(current_task_ptr->ctx, (uint8_t*)&kbuf[i], (uint64_t)buf + processed + i, 1);
            }
            kbuf[chunk] = '\0';
            puts(kbuf);
            processed += chunk;
        }
        frame->rax = count;
        return;
    }

    if (entry->type == FD_DEV) {
        char rel[256];
        if (!is_mounted_under(entry->path, "devfs", rel)) {
            frame->rax = (uint64_t)-ENODEV; return;
        }
        uint64_t res = write_devfs(rel, buf, count, entry->offset);
        if ((int64_t)res >= 0) entry->offset += res;
        frame->rax = res;
        return;
    }

    rootfs_file_t file = read_rootfs(entry->path);
    uint64_t new_size = entry->offset + count;
    if (file.size > new_size) new_size = file.size;

    void *new_data = malloc(new_size);
    if (!new_data) { frame->rax = (uint64_t)-ENOMEM; return; }

    if (file.data && file.size) memcpy(new_data, file.data, file.size);
    memcpy((uint8_t *)new_data + entry->offset, buf, count);

    int res = write_rootfs(entry->path, new_data, new_size, file.mode ? file.mode : 0644);
    free(new_data);

    if (res < 0) { frame->rax = (uint64_t)res; return; }
    entry->offset += count;
    frame->rax = count;
}

void sys_mount(syscall_frame_t *frame) {
    const char *source = (const char *)frame->rdi;
    const char *target = (const char *)frame->rsi;
    const char *filesystemtype = (const char *)frame->rdx;
    unsigned long mountflags = (unsigned long)frame->r10;
    const void *data = (const void *)frame->r8;

    (void)source; (void)mountflags; (void)data;

    if (!target || !filesystemtype || !*target || !*filesystemtype) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    rootfs_file_t dir = read_rootfs(target);
    if ((dir.mode & 0040000) == 0 && !dir.data) {
        frame->rax = (uint64_t)-ENOENT; return;
    }

    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            strncpy(mounts[i].path, target, 63); mounts[i].path[63] = '\0';
            strncpy(mounts[i].filesystemtype, filesystemtype, 31); mounts[i].filesystemtype[31] = '\0';
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

    if (!target) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t irq;
    spin_lock_irqsave(&vfs_lock, &irq);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].path, target) == 0) {
            mounts[i].active = false;
            spin_unlock_irqrestore(&vfs_lock, irq);
            frame->rax = 0;
            return;
        }
    }
    spin_unlock_irqrestore(&vfs_lock, irq);
    frame->rax = (uint64_t)-ENOENT;
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
    const char *path = (const char *)frame->rdi;
    char **user_argv = (char **)frame->rsi;
    char **user_envp = (char **)frame->rdx;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    char *argv_ptrs[64];
    char *envp_ptrs[64];

    int argc = copy_user_strarray(user_argv, argv_ptrs, 63);
    int envc = copy_user_strarray(user_envp, envp_ptrs, 63);

    int res = execve_elf(path, argv_ptrs, envp_ptrs, frame);

    free_strarray(argv_ptrs, argc);
    free_strarray(envp_ptrs, envc);

    frame->rax = (uint64_t)res;
}

void sys_chdir(syscall_frame_t *frame) {
    const char *path = (const char *)frame->rdi;
    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    rootfs_file_t dir = read_rootfs(path);
    if (!dir.data && !dir.mode) { frame->rax = (uint64_t)-ENOENT; return; }
    if ((dir.mode & 0040000) == 0 && strcmp(path, "/") != 0) {
        frame->rax = (uint64_t)-ENOTDIR; return;
    }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    strncpy(current_task_ptr->cwd, abs_path, 255);
    current_task_ptr->cwd[255] = '\0';
    frame->rax = 0;
}

void sys_ioctl(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    unsigned long req = (unsigned long)frame->rsi;
    uint64_t argp = frame->rdx;

    if (fd == 0 || fd == 1 || fd == 2) {
        if (req == TIOCGWINSZ) {
            winsize_t ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };
            write_vmm(current_task_ptr->ctx, argp, &ws, sizeof(ws));
            frame->rax = 0;
            return;
        }
    }

    frame->rax = (uint64_t)-EINVAL;
}

void sys_mkdir(syscall_frame_t *frame) {
    const char *path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    frame->rax = (uint64_t)mkdir_rootfs(path, mode);
}

void sys_getdents(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint64_t bufp = frame->rsi;
    uint64_t buflen = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    char dir_norm[256];
    strncpy(dir_norm, entry->path, sizeof(dir_norm) - 1);
    dir_norm[sizeof(dir_norm) - 1] = '\0';
    size_t dlen = strlen(dir_norm);
    if (dlen > 1 && dir_norm[dlen - 1] == '/') dir_norm[--dlen] = '\0';

    uint64_t written = 0;

    // Check if this directory is a devfs mount point
    char rel[256];
    if (is_mounted_under(entry->path, "devfs", rel)) {
        int index = (int)entry->offset;
        // Enumerate devfs devices
        while (written + sizeof(dirent_t) <= buflen) {
            const char *devname = devfs_get_device_name(index);
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

    size_t cwd_len = strlen(current_task_ptr->cwd) + 1;
    if (cwd_len > buflen) { frame->rax = (uint64_t)-ERANGE; return; }

    write_vmm(current_task_ptr->ctx, bufp, current_task_ptr->cwd, cwd_len);
    frame->rax = bufp;
}

void sys_brk(syscall_frame_t *frame) {
    uint64_t addr = frame->rdi;

    if (addr == 0) {
        // Return current break
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
                map_vmm(current_task_ptr->ctx, a, (uint64_t)pmalloc(),
                        VMM_USER | VMM_WRITABLE);
                memset_vmm(current_task_ptr->ctx, a, 0, 4096);
            }
        }
    }

    current_task_ptr->brk = addr;
    frame->rax = current_task_ptr->brk;
}

void sys_waitpid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int *wstatus = (int *)frame->rsi;

    while (1) {
        int found_child = 0;
        int zombie_found = 0;

        for (int i = 0; i < MAX_TASKS; i++) {
            // Only care about children of the current task
            if (!tasks[i].state || tasks[i].parent_pid != current_task_ptr->pid) 
                continue;
            
            // If user asked for a specific PID, ignore others
            if (pid != -1 && tasks[i].pid != pid) 
                continue;

            found_child = 1;

            if (tasks[i].state == TASK_ZOMBIE) {
                // Reap the zombie
                if (wstatus) {
                    int status = tasks[i].exit_status << 8;
                    write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int));
                }
                
                pid_t ret = tasks[i].pid;
                tasks[i].state = TASK_DEAD; // Clean up the task slot
                frame->rax = (uint64_t)ret;
                return; // Successfully reaped!
            }
        }

        // If we found no children at all, we can't wait
        if (!found_child) {
            frame->rax = -ECHILD;
            return;
        }

        // We need to yield to let the child run. But sched_lock is held
        // by syscall_handler.S during all syscalls, and the timer ISR
        // skips context switching when sched_lock != 0. So we must
        // temporarily drop the lock before yielding.
        current_task_ptr->state = TASK_READY;
        sched_lock = 0;
        asm volatile("int $32");
        sched_lock = 1;
    }
}

void sys_getpid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_getppid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->parent_pid;
}

void sys_gethostname(syscall_frame_t *frame) {
    char *name = (char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    frame->rax = get_hostname(name, len);
}

void sys_sethostname(syscall_frame_t *frame) {
    const char *name = (const char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    frame->rax = set_hostname(name, len);
}