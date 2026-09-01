#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <asm/prctl.h>
#include <linux/sched.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/fb.h>
#include <sys/statx.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/epoll.h>
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
#include <io/initrd.h>
#include <io/keyboard.h>
#include <io/pty.h>
#include <io/power.h>
#include <io/net.h>
#include <io/unix_sockets.h>
#include <io/procfs.h>
#include <io/tmpfs.h>
#include <io/ext4.h>
#include <io/iso9660.h>
#include <io/vfat.h>
#include <io/gpt.h>
#include <io/vfs.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/file.h>

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
    if (flags & O_NOFOLLOW) {
        resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), false);
        struct stat st_link;
        bool is_link = false;
        if (stat_tmpfs_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
        else if (stat_initrd_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
        else if (stat_ext4_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
        else if (stat_iso9660_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
        else if (stat_vfat_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
        if (is_link) { frame->rax = (uint64_t)-ELOOP; return; }
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    } else {
        resolve_path_symlinks(abs_path, resolved, sizeof(resolved));
        strncpy(abs_path, resolved, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    char rel_path[256];
    // Check devpts BEFORE devtmpfs: /dev/pts is a sub-path of /dev (devtmpfs), // so devtmpfs would incorrectly match /dev/pts paths with rel="pts/...".
    if (is_devpts_path(abs_path, rel_path)) {
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
    } else if (is_devtmpfs_path(abs_path, rel_path)) {
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

    {
        int ir = open_iso9660_common(abs_path, flags);
        if (ir != 1) { frame->rax = (uint64_t)ir; return; }
    }

    {
        int vr = open_fat32_common(abs_path, flags);
        if (vr != 1) { frame->rax = (uint64_t)vr; return; }
    }

    initrd_file_t file = read_initrd(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    if ((flags & O_CREAT) && (flags & O_EXCL) && file.mode) { frame->rax = (uint64_t)-EEXIST; return; }

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

    int pacc = check_parent_access(abs_path, false);
    if (pacc < 0) { frame->rax = (uint64_t)pacc; return; }

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst) || stat_tmpfs_to_kst(abs_path, &kst, true) || stat_ext4_to_kst(abs_path, &kst, true) || stat_iso9660_to_kst(abs_path, &kst, true) || stat_vfat_to_kst(abs_path, &kst, true)) {
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
    if (entry->type == FD_ISO9660) {
        struct stat kst = {0};
        int status = stat_iso9660(entry->path, &kst, true);
        if (status < 0) { frame->rax = (uint64_t)status; return; }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (entry->type == FD_VFAT) {
        struct stat kst = {0};
        int status = stat_vfat(entry->path, &kst, true);
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

    int pacc2 = check_parent_access(abs_path, false);
    if (pacc2 < 0) { frame->rax = (uint64_t)pacc2; return; }

    struct stat kst = {0};
    if (stat_virtual_device(abs_path, &kst) || stat_tmpfs_to_kst(abs_path, &kst, false) || stat_ext4_to_kst(abs_path, &kst, false) || stat_iso9660_to_kst(abs_path, &kst, false) || stat_vfat_to_kst(abs_path, &kst, false)) {
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
        if (is_devpts_path(entry->path, rel) || !is_devtmpfs_path(entry->path, rel)) {
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
    }
    else if (entry->type == FD_EXT4) {
        struct stat st;
        int status = stat_ext4(entry->path, &st, true);
        if (status < 0) { frame->rax = status; return; }
        if (st.st_size < 0) { frame->rax = -EOVERFLOW; return; }
        file_size = st.st_size;
    }
    else if (entry->type == FD_ISO9660) {
        struct stat st;
        int status = stat_iso9660(entry->path, &st, true);
        if (status < 0) { frame->rax = status; return; }
        if (st.st_size < 0) { frame->rax = -EOVERFLOW; return; }
        file_size = st.st_size;
    }
    else if (entry->type == FD_VFAT) {
        struct stat st;
        int status = stat_vfat(entry->path, &st, true);
        if (status < 0) { frame->rax = status; return; }
        if (st.st_size < 0) { frame->rax = -EOVERFLOW; return; }
        file_size = st.st_size;
    }
    else {
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
    if (entry->type == FD_ISO9660) {
        if (count == 0) { frame->rax = 0; return; }
        if (count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t got = read_iso9660(entry->path, kbuf, count, offset);
        if (got >= 0 && write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (uint64_t)got) < 0) got = -EFAULT;
        free(kbuf);
        frame->rax = (uint64_t)got;
        return;
    }
    if (entry->type == FD_VFAT) {
        if (count == 0) { frame->rax = 0; return; }
        if (count > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
        uint8_t *kbuf = malloc(count);
        if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t got = read_vfat(entry->path, kbuf, count, offset);
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
    int fd = (int)frame->rdi;
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

        int64_t got = do_read(fd, kiov[i].iov_base, kiov[i].iov_len);
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
    int fd = (int)frame->rdi;
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

        int64_t wrote = (int64_t)do_write(fd, kiov[i].iov_base, kiov[i].iov_len);
        if (wrote < 0) {
            if (total > 0) { frame->rax = total; return; }
            return;
        }

        total += (uint64_t)wrote;
        if ((uint64_t)wrote < kiov[i].iov_len) break; // short write: stop here
    }

    frame->rax = total;
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
                if (!table->entries[i]) {
                    fd_entry_t *e = malloc(sizeof(*e));
                    if (!e) { frame->rax = (uint64_t)-ENOMEM; return; }
                    *e = *entry;
                    e->open = true;
                    table->entries[i] = e;
                    retain_fd_entry(e);
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
            if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4 && entry->type != FD_ISO9660 && entry->type != FD_VFAT) { frame->rax = (uint64_t)-EBADF; return; }
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

    if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4 && entry->type != FD_ISO9660 && entry->type != FD_VFAT) { frame->rax = (uint64_t)-EBADF; return; }

    int op = operation & ~LOCK_NB;
    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN) { frame->rax = (uint64_t)-EINVAL; return; }
    int status = set_advisory_lock(entry, op == LOCK_UN ? 0 : op, (operation & LOCK_NB) != 0, false);
    frame->rax = (uint64_t)status;
}

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
    if (entry->type == FD_ISO9660) { frame->rax = (uint64_t)-EROFS; return; }
    if (entry->type == FD_VFAT) {
        struct stat st;
        if (stat_vfat(entry->path, &st, true) < 0) { frame->rax = (uint64_t)-ENOENT; return; }
        if (!can_access_stat_mode(&st, 0, 1, 0)) { frame->rax = (uint64_t)-EACCES; return; }
        int r = truncate_vfat(entry->path, length);
        if (r == 0 && entry->offset > length) entry->offset = length;
        frame->rax = (uint64_t)r;
        return;
    }

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

    if (entry->type == FD_ISO9660 || check_iso9660_path(resolved_path)) {
        struct stat st;
        int status = stat_iso9660(resolved_path, &st, true);
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
            status = get_next_iso9660_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino);
            if (status != 0) break;
            if (!emit_dirent(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 2), child_type, child)) break;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    if (entry->type == FD_VFAT || check_vfat_path(resolved_path)) {
        struct stat st;
        int status = stat_vfat(resolved_path, &st, true);
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
            status = get_next_vfat_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino);
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
    if (is_devpts_path(resolved_path, rel)) {
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

    if (is_devtmpfs_path(resolved_path, rel)) {
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
    if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4 && entry->type != FD_ISO9660 && entry->type != FD_VFAT && entry->type != FD_PROC && entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTDIR; return; }

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

    if (check_vfat_path(abs_path)) {
        frame->rax = (uint64_t)mkdir_vfat(abs_path, apply_current_umask(mode));
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

    if (check_vfat_path(abs_path)) {
        frame->rax = (uint64_t)rmdir_vfat(abs_path);
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

    if (check_vfat_path(abs_path)) {
        frame->rax = (uint64_t)unlink_vfat(abs_path);
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

    // Rock ridge symlinks live in the record system use area.
    if (check_iso9660_path(abs_path)) {
        char target[256];
        int tlen = read_iso9660_link(abs_path, target, sizeof(target));
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
    if (check_iso9660_path(abs_path)) { frame->rax = (uint64_t)-EROFS; return; }
    if (check_vfat_path(abs_path)) {
        struct stat vst;
        if (!stat_vfat_to_kst(abs_path, &vst, false)) { frame->rax = (uint64_t)-ENOENT; return; }
        if (current_task_ptr->euid != 0 && current_task_ptr->euid != vst.st_uid) { frame->rax = (uint64_t)-EPERM; return; }
        frame->rax = 0; return;
    }

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
    if (entry->type == FD_ISO9660) { frame->rax = (uint64_t)-EROFS; return; }
    if (entry->type == FD_VFAT) {
        struct stat vst;
        if (stat_vfat(entry->path, &vst, true) < 0) { frame->rax = (uint64_t)-ENOENT; return; }
        if (current_task_ptr->euid != 0 && current_task_ptr->euid != vst.st_uid) { frame->rax = (uint64_t)-EPERM; return; }
        frame->rax = 0; return;
    }

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
    if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4 && entry->type != FD_ISO9660 && entry->type != FD_VFAT) { frame->rax = (uint64_t)-EINVAL; return; }
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

    if (entry->type == FD_ISO9660) {
        uint8_t *buffer = malloc(count);
        if (!buffer) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t status = read_iso9660(entry->path, buffer, count, (uint64_t)offset);
        free(buffer);
        frame->rax = status < 0 ? (uint64_t)status : 0;
        return;
    }

    if (entry->type == FD_VFAT) {
        uint8_t *buffer = malloc(count);
        if (!buffer) { frame->rax = (uint64_t)-ENOMEM; return; }
        int64_t status = read_vfat(entry->path, buffer, count, (uint64_t)offset);
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

    if (entry->type == FD_ISO9660 || check_iso9660_path(resolved_path)) {
        struct stat st;
        int status = stat_iso9660(resolved_path, &st, true);
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
            status = get_next_iso9660_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino);
            if (status != 0) break;
            if (!emit_dirent64(bufp, &written, buflen, child_ino, (uint64_t)(child_index + 2), child_type, child)) break;
            index = child_index + 2;
            entry->offset = index;
        }
        frame->rax = written;
        return;
    }

    if (entry->type == FD_VFAT || check_vfat_path(resolved_path)) {
        struct stat st;
        int status = stat_vfat(resolved_path, &st, true);
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
            status = get_next_vfat_child(&child_index, resolved_path, child, sizeof(child), &child_type, &child_ino);
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
    if (is_devpts_path(resolved_path, rel)) {
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

    if (is_devtmpfs_path(resolved_path, rel)) {
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

    // Resolve symlinks, respecting O_NOFOLLOW for final component
    {
        char resolved[256];
        if (flags & O_NOFOLLOW) {
            resolve_path_symlinks_ex(abs_path, resolved, sizeof(resolved), false);
            struct stat st_link;
            bool is_link = false;
            if (stat_tmpfs_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
            else if (stat_initrd_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
            else if (stat_ext4_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
            else if (stat_iso9660_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
            else if (stat_vfat_to_kst(resolved, &st_link, false) && S_ISLNK(st_link.st_mode)) is_link = true;
            if (is_link) { frame->rax = (uint64_t)-ELOOP; return; }
            strncpy(abs_path, resolved, sizeof(abs_path) - 1);
            abs_path[sizeof(abs_path) - 1] = '\0';
        } else {
            resolve_path_symlinks(abs_path, resolved, sizeof(resolved));
            strncpy(abs_path, resolved, sizeof(abs_path) - 1);
            abs_path[sizeof(abs_path) - 1] = '\0';
        }
    }

    char rel_path[256];
    if (is_devtmpfs_path(abs_path, rel_path)) {
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
    } else if (is_devpts_path(abs_path, rel_path)) {
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

    {
        int ir = open_iso9660_common(abs_path, flags);
        if (ir != 1) { frame->rax = (uint64_t)ir; return; }
    }

    {
        int vr = open_fat32_common(abs_path, flags);
        if (vr != 1) { frame->rax = (uint64_t)vr; return; }
    }

    initrd_file_t file = read_initrd(abs_path);

    if (!file.mode && !(flags & O_CREAT)) { frame->rax = (uint64_t)-ENOENT; return; }

    if ((flags & O_CREAT) && (flags & O_EXCL) && file.mode) { frame->rax = (uint64_t)-EEXIST; return; }

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
        if (entry->type != FD_FILE && entry->type != FD_TMPFS && entry->type != FD_EXT4 && entry->type != FD_ISO9660 && entry->type != FD_VFAT) { frame->rax = (uint64_t)-EINVAL; return; }
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

    int pacc = check_parent_access(abs_path, false);
    if (pacc < 0) { frame->rax = (uint64_t)pacc; return; }

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
    if (stat_iso9660_to_kst(abs_path, &kst, follow_final)) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)st, &kst, sizeof(struct stat)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        frame->rax = 0;
        return;
    }
    if (stat_vfat_to_kst(abs_path, &kst, follow_final)) {
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

    if (check_iso9660_path(abs_path)) {
        char target[256];
        int tlen = read_iso9660_link(abs_path, target, sizeof(target));
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
    if (check_iso9660_path(abs_path)) { frame->rax = (uint64_t)-EROFS; return; }

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
            status = stat_tmpfs_to_kst(cwd, &kst, true) || stat_ext4_to_kst(cwd, &kst, true) || stat_iso9660_to_kst(cwd, &kst, true) || stat_vfat_to_kst(cwd, &kst, true) || stat_proc(cwd, cwd, &kst, true) || stat_initrd_to_kst(cwd, &kst, true) || stat_virtual_device(cwd, &kst) ? 0 : -ENOENT;
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

    if (!((stat_virtual_device(abs_path, &kst)) || (follow_final ? stat_tmpfs_to_kst(abs_path, &kst, true) : stat_tmpfs_to_kst(abs_path, &kst, false)) || (stat_ext4_to_kst(abs_path, &kst, follow_final)) || (stat_iso9660_to_kst(abs_path, &kst, follow_final)) || (stat_vfat_to_kst(abs_path, &kst, follow_final)) || (stat_proc(abs_path, path, &kst, follow_final)))) {
        if (!stat_initrd_to_kst(abs_path, &kst, follow_final)) { frame->rax = (uint64_t)-ENOENT; return; }
    }

    struct statx ksx;
    stat_to_statx(&ksx, &kst, abs_path);
    statx_add_fs_metadata(&ksx, abs_path, follow_final, mask);
    if (write_vmm(current_task_ptr->ctx, (uint64_t)sx, &ksx, sizeof(ksx)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}
