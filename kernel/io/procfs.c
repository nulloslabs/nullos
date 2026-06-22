#include <freestanding/stdint.h>
#include <freestanding/dirent.h>
#include <main/string.h>
#include <main/scheduler.h>
#include <main/fd.h>
#include <io/devtmpfs.h>
#include <io/procfs.h>
#include <mm/vma.h>
#include <syscalls/syscall_impls.h>

const proc_static_node_t proc_nodes[] = {
    { "",              PROC_NODE_DIR,     PROC_DIR_ROOT        },
    { "/self",         PROC_NODE_SYMLINK, PROC_LINK_SELF       },
    { "/mounts",       PROC_NODE_SYMLINK, PROC_LINK_ROOT_MOUNTS},
    { "/<pid>",        PROC_NODE_DIR,     PROC_DIR_PID         },
    { "/<pid>/fd",     PROC_NODE_DIR,     PROC_DIR_FD          },
    { "/<pid>/maps",   PROC_NODE_FILE,    PROC_FILE_MAPS       },
    { "/<pid>/mounts", PROC_NODE_FILE,    PROC_FILE_MOUNTS     },
    { "/<pid>/exe",    PROC_NODE_SYMLINK, PROC_LINK_EXE        },
    { "/<pid>/cwd",    PROC_NODE_SYMLINK, PROC_LINK_CWD        },
    { "/<pid>/fd/<n>", PROC_NODE_SYMLINK, PROC_LINK_FD         },
};

const dirent_static_t root_children[] = {
    { "self",   DT_LNK },
    { "mounts", DT_LNK },
};

const dirent_static_t pid_children[] = {
    { "fd",     DT_DIR },
    { "maps",   DT_REG },
    { "mounts", DT_REG },
    { "exe",    DT_LNK },
    { "cwd",    DT_LNK },
};

static bool starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static const char *parse_num(const char *p, int *out) {
    if (*p < '0' || *p > '9') return NULL;
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *out = v;
    return p;
}

static int task_index_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].state != TASK_ZOMBIE &&
            tasks[i].pid == pid)
            return i;
    }
    return -1;
}

static int nth_open_fd(int pid_idx, int n) {
    int count = 0;
    for (int fd = 0; fd < FD_MAX; fd++) {
        if (tasks[pid_idx].fd_table.entries[fd].open) {
            if (count == n) return fd;
            count++;
        }
    }
    return -1;
}

static bool match_pattern(const char *pattern, const char *path, int self, int *pid_idx, int *fd_num) {
    const char *pa = pattern;
    const char *s  = path;

    while (*pa) {
        if (starts_with(pa, "<pid>")) {
            int raw_pid;
            const char *after = parse_num(s, &raw_pid);
            if (!after && starts_with(s, "self") && (s[4] == '/' || s[4] == '\0')) {
                *pid_idx = self;
                s += 4;
            } else if (after) {
                *pid_idx = task_index_by_pid(raw_pid);
                if (*pid_idx < 0) return false;
                s = after;
            } else {
                return false;
            }
            pa += 5;  // skip "<pid>"
        } else if (starts_with(pa, "<n>")) {
            const char *after = parse_num(s, fd_num);
            if (!after) return false;
            s = after;
            pa += 3;
        } else {
            if (*s++ != *pa++) return false;
        }
    }
    return *s == '\0';
}

static void buf_append(char *buf, size_t *pos, size_t cap, const char *s) {
    size_t len = strlen(s);
    if (*pos + len >= cap) len = cap - 1 - *pos;
    if (!len) return;
    memcpy(buf + *pos, s, len);
    *pos += len;
    buf[*pos] = '\0';
}

static void buf_append_hex(char *buf, size_t *pos, size_t cap, uint64_t v) {
    char tmp[32]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        uint64_t d = v & 0xF;
        tmp[n++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        v >>= 4;
    }
    char fwd[32];
    for (int k = 0; k < n; k++) fwd[k] = tmp[n - 1 - k];
    fwd[n] = '\0';
    buf_append(buf, pos, cap, fwd);
}

static size_t build_maps(int pid_idx, char *out) {
    size_t pos = 0; out[0] = '\0';
    extern const vma_table_t *task_vma_table(int pid_idx);
    const vma_table_t *tbl = task_vma_table(pid_idx);
    if (!tbl) return 0;
    for (int i = 0; ; i++) {
        vma_t v;
        if (!get_vma(tbl, i, &v)) break;
        buf_append_hex(out, &pos, PROCFS_MAX_CONTENT, v.start);
        buf_append(out, &pos, PROCFS_MAX_CONTENT, "-");
        buf_append_hex(out, &pos, PROCFS_MAX_CONTENT, v.end);
        buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, (v.prot & VMA_PROT_READ)    ? "r" : "-");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, (v.prot & VMA_PROT_WRITE)   ? "w" : "-");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, (v.prot & VMA_PROT_EXEC)    ? "x" : "-");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, (v.flags & VMA_FLAG_SHARED) ? "s" : "p");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
        buf_append_hex(out, &pos, PROCFS_MAX_CONTENT, v.offset);
        buf_append(out, &pos, PROCFS_MAX_CONTENT, " 00:00 0 ");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, v.name[0] ? v.name : "");
        buf_append(out, &pos, PROCFS_MAX_CONTENT, "\n");
    }
    return pos;
}

static size_t build_mounts(char *out) {
    size_t pos = 0; out[0] = '\0';
    for (int i = 0; ; i++) {
        char line[160];
        if (enumerate_vfs_mounts(i, line, sizeof(line)) <= 0) break;
        buf_append(out, &pos, PROCFS_MAX_CONTENT, line);
        buf_append(out, &pos, PROCFS_MAX_CONTENT, "\n");
    }
    return pos;
}

static int copy_str(const char *src, char *out, size_t out_size) {
    if (!src || !src[0]) return -1;
    size_t len = strlen(src);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, src, len);
    out[len] = '\0';
    return (int)len;
}

static int fmt_int(int v, char *out, size_t out_size) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    if (n >= (int)out_size) return -1;
    for (int k = 0; k < n; k++) out[k] = tmp[n - 1 - k];
    out[n] = '\0';
    return n;
}

size_t procfs_content(const proc_node_t *node, char *out) {
    switch (node->entry) {
    case PROC_FILE_MAPS:   return build_maps(node->pid, out);
    case PROC_FILE_MOUNTS: return build_mounts(out);
    default:               return 0;
    }
}

static void write_name(char *name, size_t name_size, const char *src) {
    strncpy(name, src, name_size - 1);
    name[name_size - 1] = '\0';
}

bool procfs_is_proc_path(const char *abs_path) {
    if (!abs_path) return false;
    if (strcmp(abs_path, "/proc") == 0) return true;
    return starts_with(abs_path, "/proc/");
}

bool procfs_resolve(const char *abs_path, int self, proc_node_t *out) {
    out->type   = PROC_NODE_NONE;
    out->entry  = PROC_NONE;
    out->pid    = -1;
    out->fd_num = -1;

    if (!starts_with(abs_path, "/proc")) return false;

    // The part after "/proc" (may be empty string for "/proc" itself).
    const char *rel = abs_path + 5;
    if (strcmp(rel, "/") == 0) rel = "";  // treat "/proc/" same as "/proc"

    // Resolve /proc/self -> /proc/<pid> so that all <pid> patterns work.
    char resolved[256];
    if (starts_with(rel, "/self/") || strcmp(rel, "/self") == 0) {
        char pidbuf[16];
        fmt_int(tasks[self].pid, pidbuf, sizeof(pidbuf));
        const char *rest = rel + 5;  // skip "/self"
        size_t pos = 0;
        resolved[pos++] = '/';
        for (const char *p = pidbuf; *p; p++) resolved[pos++] = *p;
        for (const char *p = rest; *p; p++) resolved[pos++] = *p;
        resolved[pos] = '\0';
        rel = resolved;
    }

    for (int i = 0; i < PROC_NODE_COUNT; i++) {
        int pid_idx = -1, fd_num = -1;
        if (!match_pattern(proc_nodes[i].pattern, rel, self, &pid_idx, &fd_num))
            continue;

        // Validate pid if one was parsed.
        if (pid_idx >= 0) {
            if (pid_idx >= MAX_TASKS) return false;
            if (tasks[pid_idx].state == TASK_DEAD ||
                tasks[pid_idx].state == TASK_ZOMBIE) return false;
        }

        // Validate fd if one was parsed.
        if (fd_num >= 0) {
            if (fd_num >= FD_MAX) return false;
            if (!tasks[pid_idx].fd_table.entries[fd_num].open) return false;
        }

        out->type   = proc_nodes[i].type;
        out->entry  = proc_nodes[i].entry;
        out->pid    = pid_idx;
        out->fd_num = fd_num;
        return true;
    }
    return false;
}

bool procfs_is_dir(const proc_node_t *node) {
    return node->type == PROC_NODE_DIR;
}

int procfs_readlink(const proc_node_t *node, int self, char *out, size_t out_size) {
    switch (node->entry) {
    case PROC_LINK_SELF:
        return fmt_int(tasks[self].pid, out, out_size);
    case PROC_LINK_ROOT_MOUNTS:
        return copy_str("/proc/self/mounts", out, out_size);
    case PROC_LINK_EXE:
        return copy_str(tasks[node->pid].exe, out, out_size);
    case PROC_LINK_CWD:
        return copy_str(tasks[node->pid].cwd, out, out_size);
    case PROC_LINK_FD: {
        fd_entry_t *e = &tasks[node->pid].fd_table.entries[node->fd_num];
        if (!e->open) return -1;
        return copy_str(e->path, out, out_size);
    }
    default:
        return -1;
    }
}

bool procfs_get_dirent(const proc_node_t *dir, int self, int index, char *name, size_t name_size, uint8_t *type_out) {
    (void)self;
    if (index < 0) return false;

    switch (dir->entry) {
    case PROC_DIR_ROOT: {
        // Static entries first (self, mounts), then one dir per live task.
        int n_static = (int)(sizeof(root_children) / sizeof(root_children[0]));
        if (index < n_static) {
            write_name(name, name_size, root_children[index].name);
            *type_out = root_children[index].dt_type;
            return true;
        }
        int target = index - n_static, count = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_DEAD || tasks[i].state == TASK_ZOMBIE) continue;
            if (tasks[i].pid == 0) continue;
            if (count++ == target) {
                fmt_int(tasks[i].pid, name, name_size);
                *type_out = DT_DIR;
                return true;
            }
        }
        return false;
    }

    case PROC_DIR_PID: {
        int n = (int)(sizeof(pid_children) / sizeof(pid_children[0]));
        if (index >= n) return false;
        write_name(name, name_size, pid_children[index].name);
        *type_out = pid_children[index].dt_type;
        return true;
    }

    case PROC_DIR_FD: {
        int fd = nth_open_fd(dir->pid, index);
        if (fd < 0) return false;
        fmt_int(fd, name, name_size);
        *type_out = DT_LNK;
        return true;
    }

    default:
        return false;
    }
}
