#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>
#include <freestanding/signal.h>
#include <freestanding/fcntl.h>
#include <freestanding/sys/ioctl.h>
#include <freestanding/sys/fb.h>
#include <freestanding/sys/stat.h>
#include <freestanding/time.h>
#include <freestanding/wait.h>
#include <freestanding/termios.h>
#include <main/limine_req.h>
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
#include <main/devfs.h>
#include <main/spinlock.h>
#include <main/elf.h>
#include <main/halt.h>
#include <main/hostname.h>
#include <main/uname.h>
#include <main/acpi.h>
#include <main/msr.h>
#include <io/tty.h>
#include <io/pty.h>
#include <io/hpet.h>

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
} dirent_t;

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define RB_REBOOT 0x00
#define RB_POWEROFF 0x01
#define RB_HALT 0x02

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

static void build_abs_path(const char *path, char *out, size_t out_size) {
    build_abs_path_at(AT_FDCWD, path, out, out_size);
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

static int ptm_path_idx(const char *path) {
    if (path[0]!='p'||path[1]!='t'||path[2]!='m'||path[3]!=':') return -1;
    const char *n = path + 4;
    int idx = 0;
    while (*n >= '0' && *n <= '9') idx = idx * 10 + (*n++ - '0');
    return idx;
}

static void free_strarray(char **arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i]);
}

void sys_read(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    uint8_t *buf = (uint8_t *)frame->rsi;
    uint64_t count = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    if (fd == 0) {
        if (stdin_buf_pos >= stdin_buf_len) {
            stdin_buf_len = 0;
            stdin_buf_pos = 0;
            reset_term_line_start();
            while (stdin_buf_len < (int)sizeof(stdin_buf) - 1) {
                current_task_ptr->state = TASK_READY;
                sched_lock = 0;
                char c = getc();
                sched_lock = 1;
                current_task_ptr->state = TASK_RUNNING;

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

    if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        int got = read_pty_master(idx, (char *)buf, (int)count);
        frame->rax = (got < 0) ? (uint64_t)-EBADF : (uint64_t)got;
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

    if (entry->type == FD_STREAM) {
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

    if (entry->type == FD_PTY_MASTER) {
        int idx = ptm_path_idx(entry->path);
        int w = write_pty_master(idx, (const char *)buf, (int)count);
        frame->rax = (w < 0) ? (uint64_t)-EBADF : (uint64_t)w;
        return;
    }

    rootfs_file_t file = read_rootfs(entry->path);
    if (!can_access_rootfs(&file, 0, 1, 0)) {
        frame->rax = (uint64_t)-EACCES;
        return;
    }

    uint64_t new_size = entry->offset + count;
    if (file.size > new_size) new_size = file.size;

    void *new_data = malloc(new_size);
    if (!new_data) { frame->rax = (uint64_t)-ENOMEM; return; }

    if (file.data && file.size) memcpy(new_data, file.data, file.size);
    memcpy((uint8_t *)new_data + entry->offset, buf, count);

    int res = write_rootfs(entry->path, new_data, new_size,
                           file.mode ? file.mode : 0644,
                           file.mode ? file.uid : current_task_ptr->euid,
                           file.mode ? file.gid : current_task_ptr->egid);
    free(new_data);

    if (res < 0) { frame->rax = (uint64_t)res; return; }
    entry->offset += count;
    frame->rax = count;
}

extern void register_pts_device(int idx);
extern void unregister_pts_device(int idx);

void sys_open(syscall_frame_t *frame) {
    const char *path = (const char *)frame->rdi;
    uint32_t flags = (uint32_t)frame->rsi;
    mode_t mode = (mode_t)frame->rdx;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

    char rel_path[256];
    if (is_mounted_under(abs_path, "devfs", rel_path)) {
        if (!devfs_device_exists(rel_path)) { frame->rax = (uint64_t)-ENOENT; return; }
        // ptmx: allocate a PTY and return a master fd
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
        if (pty_idx >= 0) {
            int r = open_pty_slave(pty_idx);
            if (r < 0) { frame->rax = (uint64_t)r; return; }
        }
        int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_DEV, flags);
        if (fd < 0 && pty_idx >= 0)
            release_pty_slave(pty_idx);
        frame->rax = (uint64_t)fd;
        return;
    }

    rootfs_file_t file = read_rootfs(abs_path);

    if (!file.mode && !(flags & O_CREAT)) {
        frame->rax = (uint64_t)-ENOENT;
        return;
    }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(abs_path, "", 0, mode, current_task_ptr->euid, current_task_ptr->egid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
        file = read_rootfs(abs_path);
    }

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);
    if (!can_access_rootfs(&file, want_read, want_write, 0)) {
        frame->rax = (uint64_t)-EACCES;
        return;
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
    const char *path = (const char *)frame->rdi;
    struct stat *st = (struct stat *)frame->rsi;

    if (!path || !st) { frame->rax = (uint64_t)-EINVAL; return; }

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
    if (entry->type == FD_STREAM) { frame->rax = -ESPIPE; return; }

    switch (whence) {
        case 0: // SEEK_SET
            entry->offset = (uint64_t)offset;
            break;
        case 1: // SEEK_CUR
            entry->offset = (uint64_t)((int64_t)entry->offset + offset);
            break;
        case 2: { // SEEK_END
            rootfs_file_t file = read_rootfs(entry->path);
            if (!file.data) { frame->rax = -ENOENT; return; }
            entry->offset = (uint64_t)((int64_t)file.size + offset);
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
    // fd (r8) and offset (r9) ignored — anonymous only for now

    (void)addr; (void)flags;

    if (length == 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if ((prot & 2) && (prot & 4)) {
        frame->rax = (uint64_t)-EACCES;
        return;
    }

    uint64_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t vmm_flags = VMM_USER;
    if (prot & 2) vmm_flags |= VMM_WRITABLE;
    if (!(prot & 4)) vmm_flags |= VMM_NX;

    void *ptr = vmalloc_ex(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    if (!ptr) {
        frame->rax = (uint64_t)-ENOMEM;
        return;
    }

    frame->rax = (uint64_t)ptr;
}

void sys_mprotect(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;

    if (length == 0) {
        frame->rax = 0;
        return;
    }
    if ((prot & 2) && (prot & 4)) {
        frame->rax = (uint64_t)-EACCES;
        return;
    }

    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (addr + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t phys = get_vmm_phys(current_task_ptr->ctx, a);
        if (!phys) {
            frame->rax = (uint64_t)-ENOMEM;
            return;
        }
        uint64_t vmm_flags = VMM_USER;
        if (prot & 2) vmm_flags |= VMM_WRITABLE;
        if (!(prot & 4)) vmm_flags |= VMM_NX;
        map_vmm(current_task_ptr->ctx, a, phys, vmm_flags);
    }

    frame->rax = 0;
}

void sys_munmap(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;

    if (length == 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (addr + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        unmap_vmm(current_task_ptr->ctx, a);
    }

    frame->rax = 0;
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
                        VMM_USER | VMM_WRITABLE | VMM_NX);
                memset_vmm(current_task_ptr->ctx, a, 0, 4096);
            }
        }
    }

    current_task_ptr->brk = addr;
    frame->rax = current_task_ptr->brk;
}

void sys_ioctl(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    unsigned long req = (unsigned long)frame->rsi;
    uint64_t argp = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);

    // Handle framebuffer ioctl requests
    if (entry && entry->type == FD_DEV) {
        char rel[256];
        if (is_mounted_under(entry->path, "devfs", rel)) {
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

    // Also treat devfs tty devices as ttys
    if (!is_tty) {
        if (entry && entry->type == FD_DEV) {
            char rel[256];
            if (is_mounted_under(entry->path, "devfs", rel))
                if (strncmp(rel, "tty", 3) == 0 || strncmp(rel, "pts/", 4) == 0 || strcmp(rel, "console") == 0)
                    is_tty = 1;
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
            if (is_mounted_under(entry->path, "devfs", rel)) {
                if (strncmp(rel, "tty", 3) == 0) {
                     int idx = rel[3] - '0';
                     if (idx >= 0 && idx < NUM_TTYS) {
                         t = get_tty(idx)->termios;
                     }
                } else if (strncmp(rel, "pts/", 4) == 0) {
                    t.c_iflag = 0x0500;
                    t.c_oflag = 0x0005;
                    t.c_cflag = 0x04BF;
                    t.c_lflag = 0x8A3B;
                    t.c_cc[4] = 1;
                }
            }
        }
        write_vmm(current_task_ptr->ctx, argp, &t, sizeof(t));
        frame->rax = 0;
        return;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        // Accept termios writes silently — we don't implement full termios
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
        // Report bytes available in stdin buffer; 0 for everything else
        int avail = (fd == 0) ? (stdin_buf_len - stdin_buf_pos) : 0;
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
            entry->flags = (uint32_t)arg;
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
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

    struct timespec ts;
    read_vmm(current_task_ptr->ctx, &ts, (uint64_t)req, sizeof(struct timespec));

    uint64_t total_us = (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
    sleep_us(total_us);

    if (rem) {
        struct timespec zero_ts = {0, 0};
        write_vmm(current_task_ptr->ctx, (uint64_t)rem, &zero_ts, sizeof(struct timespec));
    }

    frame->rax = 0;
}

void sys_getpid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
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
    const char *path = (const char *)frame->rdi;
    char **user_argv = (char **)frame->rsi;
    char **user_envp = (char **)frame->rdx;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    rootfs_file_t file = read_rootfs(path);
    if (file.mode && !can_access_rootfs(&file, 0, 0, 1)) {
        frame->rax = (uint64_t)-EACCES;
        return;
    }

    char *argv_ptrs[64];
    char *envp_ptrs[64];

    int argc = copy_user_strarray(user_argv, argv_ptrs, 63);
    int envc = copy_user_strarray(user_envp, envp_ptrs, 63);

    int res = execve_elf(path, argv_ptrs, envp_ptrs, frame);
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

void sys_waitpid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int *wstatus = (int *)frame->rsi;
    int options = (int)frame->rdx;

    while (1) {
        int found_child = 0;

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

        // WNOHANG: return 0 if no child has exited
        if (options & WNOHANG) {
            frame->rax = 0;
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

void sys_kill(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;

    if (pid <= 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    if (sig < 0 || sig > 10) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            // Permission check: root or same UID
            if (!current_task_ptr && !current_task_ptr->euid == 0 && current_task_ptr->uid != tasks[i].uid) {
                frame->rax = (uint64_t)-EPERM;
                return;
            }

            if (pid == 1 && sig != 0) {
                frame->rax = (uint64_t)-EPERM;
                return;
            }

            if (sig == 0) {
                frame->rax = 0;
                return;
            }

            switch (sig) {
                case SIGSTOP:
                case SIGTSTP:
                    if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_READY) {
                        tasks[i].state = TASK_STOPPED;
                        if (pid == current_task_ptr->pid) {
                            asm volatile("int $32");
                        }
                    }
                    break;
                case SIGCONT:
                    if (tasks[i].state == TASK_STOPPED) {
                        tasks[i].state = TASK_READY;
                    }
                    break;
                default:
                    // SIGKILL, SIGSEGV, SIGINT, SIGTERM, SIGABRT, SIGILL, SIGHUP
                    if (pid == current_task_ptr->pid) {
                        exit_task(128 + sig);
                        // exit_task does not return
                    }

                    tasks[i].state = TASK_ZOMBIE;
                    tasks[i].exit_status = 128 + sig;

                    // Reparent children to init
                    for (int j = 1; j < MAX_TASKS; j++) {
                        if (tasks[j].state != TASK_DEAD && tasks[j].parent_pid == pid) {
                            if (tasks[j].state == TASK_ZOMBIE) {
                                tasks[j].state = TASK_DEAD;
                            } else {
                                tasks[j].parent_pid = 1;
                            }
                        }
                    }

                    // Close file descriptors
                    for (int j = 0; j < FD_MAX; j++) {
                        if (tasks[i].fd_table.entries[j].open) {
                            free_fd(&tasks[i].fd_table, j);
                        }
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

void sys_chdir(syscall_frame_t *frame) {
    const char *path = (const char *)frame->rdi;
    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    char abs_path[256];
    build_abs_path(path, abs_path, sizeof(abs_path));

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
    const char *path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    frame->rax = (uint64_t)mkdir_rootfs(path, mode, current_task_ptr->euid, current_task_ptr->egid);
}

void sys_chmod(syscall_frame_t *frame) {
    const char *path = (const char *)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;
    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    frame->rax = (uint64_t)chmod_rootfs(path, mode);
}

void sys_fchmod(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    mode_t mode = (mode_t)frame->rsi;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }

    frame->rax = (uint64_t)chmod_rootfs(entry->path, mode);
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
    frame->rax = (uint64_t)-ENOSYS; // Added back stub to ensure compliance
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

void sys_arch_prctl(syscall_frame_t *frame) {
    int code = (int)frame->rdi;
    uint64_t addr = frame->rsi;

    switch (code) {
        case ARCH_SET_FS:
            current_task_ptr->fs_base = addr;
            write_msr(MSR_FS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_FS:
            if (!addr) { frame->rax = (uint64_t)-EFAULT; return; }
            write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->fs_base, sizeof(uint64_t));
            frame->rax = 0;
            return;
        case ARCH_SET_GS:
            current_task_ptr->gs_base = addr;
            write_msr(MSR_GS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_GS:
            if (!addr) { frame->rax = (uint64_t)-EFAULT; return; }
            write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->gs_base, sizeof(uint64_t));
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_mount(syscall_frame_t *frame) {
    const char *source = (const char *)frame->rdi;
    const char *target = (const char *)frame->rsi;
    const char *filesystemtype = (const char *)frame->rdx;
    unsigned long mountflags = (unsigned long)frame->r10;
    const void *data = (const void *)frame->r8;

    (void)source; (void)mountflags; (void)data;

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

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

    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }

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

void sys_sethostname(syscall_frame_t *frame) {
    const char *name = (const char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    bool priv = current_task_ptr && current_task_ptr->euid == 0;
    if (!priv) { frame->rax = (uint64_t)-EPERM; return; }
    frame->rax = set_hostname(name, len);
}

void sys_gethostname(syscall_frame_t *frame) {
    char *name = (char *)frame->rdi;
    size_t len = (size_t)frame->rsi;
    frame->rax = get_hostname(name, len);
}

void sys_openat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *path = (const char *)frame->rsi;
    uint32_t flags = (uint32_t)frame->rdx;
    mode_t mode = (mode_t)frame->r10;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    char rel_path[256];
    if (is_mounted_under(abs_path, "devfs", rel_path)) {
        if (!devfs_device_exists(rel_path)) { frame->rax = (uint64_t)-ENOENT; return; }
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
        if (pty_idx >= 0) {
            int r = open_pty_slave(pty_idx);
            if (r < 0) { frame->rax = (uint64_t)r; return; }
        }
        int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_DEV, flags);
        if (fd < 0 && pty_idx >= 0)
            release_pty_slave(pty_idx);
        frame->rax = (uint64_t)fd;
        return;
    }

    rootfs_file_t file = read_rootfs(abs_path);

    if (!file.mode && !(flags & O_CREAT)) {
        frame->rax = (uint64_t)-ENOENT;
        return;
    }

    if ((flags & O_CREAT) && !file.data && !file.mode) {
        int r = write_rootfs(abs_path, "", 0, mode, current_task_ptr->euid, current_task_ptr->egid);
        if (r < 0) { frame->rax = (uint64_t)r; return; }
        file = read_rootfs(abs_path);
    }

    int want_write = (flags & O_WRONLY) || (flags & O_RDWR);
    int want_read = !want_write || (flags & O_RDWR);
    if (!can_access_rootfs(&file, want_read, want_write, 0)) {
        frame->rax = (uint64_t)-EACCES;
        return;
    }

    int fd = alloc_fd(&current_task_ptr->fd_table, abs_path, FD_FILE, flags);
    frame->rax = (uint64_t)fd;
}

void sys_fchmodat(syscall_frame_t *frame) {
    int dirfd = (int)frame->rdi;
    const char *path = (const char *)frame->rsi;
    mode_t mode = (mode_t)frame->rdx;
    int flags = (int)frame->r10;

    (void)flags;

    if (!path) { frame->rax = (uint64_t)-EINVAL; return; }

    char abs_path[256];
    int res = build_abs_path_at(dirfd, path, abs_path, sizeof(abs_path));
    if (res < 0) { frame->rax = (uint64_t)res; return; }

    frame->rax = (uint64_t)chmod_rootfs(abs_path, mode);
}
