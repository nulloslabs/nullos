#include <freestanding/stdint.h>
#include <freestanding/cpuid.h>
#include <freestanding/dirent.h>
#include <main/string.h>
#include <main/sched.h>
#include <main/fd.h>
#include <main/mp.h>
#include <main/machine_info.h>
#include <io/devtmpfs.h>
#include <io/procfs.h>
#include <mm/vma.h>
#include <syscalls/syscall_impls.h>

const proc_static_node_t proc_nodes[] = {
    { "",              PROC_NODE_DIR,     PROC_DIR_ROOT         },
    { "/self",         PROC_NODE_SYMLINK, PROC_LINK_SELF        },
    { "/mounts",       PROC_NODE_SYMLINK, PROC_LINK_ROOT_MOUNTS },
    { "/cpuinfo",      PROC_NODE_FILE,    PROC_FILE_CPUINFO     },
    { "/<pid>",        PROC_NODE_DIR,     PROC_DIR_PID          },
    { "/<pid>/fd",     PROC_NODE_DIR,     PROC_DIR_FD           },
    { "/<pid>/maps",   PROC_NODE_FILE,    PROC_FILE_MAPS        },
    { "/<pid>/mounts", PROC_NODE_FILE,    PROC_FILE_MOUNTS      },
    { "/<pid>/auxv",   PROC_NODE_FILE,    PROC_FILE_AUXV        },
    { "/<pid>/exe",    PROC_NODE_SYMLINK, PROC_LINK_EXE         },
    { "/<pid>/cwd",    PROC_NODE_SYMLINK, PROC_LINK_CWD         },
    { "/<pid>/fd/<n>", PROC_NODE_SYMLINK, PROC_LINK_FD          },
};

const dirent_static_t root_children[] = {
    { "self",    DT_LNK },
    { "mounts",  DT_LNK },
    { "cpuinfo", DT_REG },
};

const dirent_static_t pid_children[] = {
    { "fd",     DT_DIR },
    { "maps",   DT_REG },
    { "mounts", DT_REG },
    { "auxv",   DT_REG },
    { "exe",    DT_LNK },
    { "cwd",    DT_LNK },
};

static int fmt_int(int v, char *out, size_t out_size) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    if (n >= (int)out_size) return -1;
    for (int k = 0; k < n; k++) out[k] = tmp[n - 1 - k];
    out[n] = '\0';
    return n;
}

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

static size_t build_auxv(int pid_idx, char *out) {
    int words = tasks[pid_idx].auxv_blob_words;
    if (words <= 0) return 0;
    size_t size = (size_t)words * sizeof(uint64_t);
    if (size > PROCFS_MAX_CONTENT) size = PROCFS_MAX_CONTENT;
    memcpy(out, tasks[pid_idx].auxv_blob, size);
    return size;
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

static void buf_append_uint(char *buf, size_t *pos, size_t cap, uint32_t v) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    char fwd[16];
    for (int k = 0; k < n; k++) fwd[k] = tmp[n - 1 - k];
    fwd[n] = '\0';
    buf_append(buf, pos, cap, fwd);
}

static size_t build_cpuinfo(char *out) {
    size_t pos = 0; out[0] = '\0';
    size_t cap = PROCFS_MAX_CONTENT;

    const char *vendor   = get_cpu_vendor();
    const char *model_nm = get_cpu_name();
    uint32_t family      = get_cpu_family();
    uint32_t model       = get_cpu_model();
    uint32_t stepping    = get_cpu_stepping();
    uint32_t cores       = get_cpu_cores();
    uint32_t threads     = get_cpu_threads();
    uint32_t freq        = get_cpu_freq();

    // Some extra stuff
    uint32_t clflush_size = 64;
    uint32_t cache_alignment = 64;
    uint32_t phys_bits = 32;
    uint32_t virt_bits = 32;

    // Clamp silently, freq can be 0 if HPET not present
    if (!cores || cores > (uint32_t)cpu_count) cores = (uint32_t)cpu_count;
    if (!threads || threads > (uint32_t)cpu_count) threads = (uint32_t)cpu_count;

    // Build the flags string from real CPUID data
    static const struct { cpu_feature_t feat; const char *name; } flag_map[] = {
        { CPU_FEATURE_FPU,    "fpu"    },
        { CPU_FEATURE_SSE,    "sse"    },
        { CPU_FEATURE_SSE2,   "sse2"   },
        { CPU_FEATURE_SSE3,   "pni"    },
        { CPU_FEATURE_SSSE3,  "ssse3"  },
        { CPU_FEATURE_SSE41,  "sse4_1" },
        { CPU_FEATURE_SSE42,  "sse4_2" },
        { CPU_FEATURE_AVX,    "avx"    },
        { CPU_FEATURE_AVX2,   "avx2"   },
        { CPU_FEATURE_POPCNT, "popcnt" },
        { CPU_FEATURE_AES,    "aes"    },
        { CPU_FEATURE_NX,     "nx"     },
        { CPU_FEATURE_XSAVE,  "xsave"  },
    };
    int flag_count = (int)(sizeof(flag_map) / sizeof(flag_map[0]));

    char freq_str[32];
    {
        uint32_t mhz = freq;
        char tmp[16]; int n = 0;
        if (mhz == 0) { tmp[n++] = '0'; }
        while (mhz > 0) { tmp[n++] = '0' + (mhz % 10); mhz /= 10; }
        int k;
        for (k = 0; k < n; k++) freq_str[k] = tmp[n - 1 - k];
        freq_str[k++] = '.';
        freq_str[k++] = '0';
        freq_str[k++] = '0';
        freq_str[k++] = '0';
        freq_str[k]   = '\0';
    }

    {
        unsigned int eax, ebx, ecx, edx;
        unsigned int max_standard_leaf = 0;
        unsigned int max_extended_leaf = 0;

        __cpuid(0, max_standard_leaf, ebx, ecx, edx);
        if (max_standard_leaf >= 1) {
            __cpuid(1, eax, ebx, ecx, edx);
            unsigned int clflush_chunks = (ebx >> 8) & 0xFF;
            if (clflush_chunks > 0) {
                clflush_size = clflush_chunks * 8;
                cache_alignment = clflush_size;
            }
        }

        __cpuid(0x80000000, max_extended_leaf, ebx, ecx, edx);
        if (max_extended_leaf >= 0x80000008) {
            __cpuid(0x80000008, eax, ebx, ecx, edx);
            phys_bits = eax & 0xFF;
            virt_bits = (eax >> 8) & 0xFF;
        }
    }

    for (int i = 0; i < cpu_count; i++) {
        char num[16];
        fmt_int(i, num, sizeof(num));

        buf_append(out, &pos, cap, "processor\t: "); buf_append(out, &pos, cap, num); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "vendor_id\t: "); buf_append(out, &pos, cap, vendor[0] ? vendor : "unknown"); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "cpu family\t: "); buf_append_uint(out, &pos, cap, family); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "model\t\t: "); buf_append_uint(out, &pos, cap, model); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "model name\t: "); buf_append(out, &pos, cap, model_nm[0] ? model_nm : "Unknown"); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "stepping\t: "); buf_append_uint(out, &pos, cap, stepping); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "cpu MHz\t\t: "); buf_append(out, &pos, cap, freq_str); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "physical id\t: 0\n");
        buf_append(out, &pos, cap, "siblings\t: "); buf_append_uint(out, &pos, cap, threads); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "core id\t\t: "); buf_append_uint(out, &pos, cap, (uint32_t)i % cores); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "cpu cores\t: "); buf_append_uint(out, &pos, cap, cores); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "apicid\t\t: "); buf_append_uint(out, &pos, cap, (uint32_t)i); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "initial apicid\t: "); buf_append_uint(out, &pos, cap, (uint32_t)i); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "fpu\t\t: "); buf_append(out, &pos, cap, cpu_has_feature(CPU_FEATURE_FPU) ? "yes" : "no"); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "fpu_exception\t: "); buf_append(out, &pos, cap, cpu_has_feature(CPU_FEATURE_FPU) ? "yes" : "no"); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "wp\t\t: yes\n");
        buf_append(out, &pos, cap, "flags\t\t:");
        for (int f = 0; f < flag_count; f++) {
            if (cpu_has_feature(flag_map[f].feat)) {
                buf_append(out, &pos, cap, " ");
                buf_append(out, &pos, cap, flag_map[f].name);
            }
        }
        buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "clflush size\t: ");  buf_append_uint(out, &pos, cap, clflush_size); buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "cache_alignment\t: ");  buf_append_uint(out, &pos, cap, cache_alignment);  buf_append(out, &pos, cap, "\n");
        buf_append(out, &pos, cap, "address sizes\t: "); buf_append_uint(out, &pos, cap, phys_bits);  buf_append(out, &pos, cap, " bits physical, "); buf_append_uint(out, &pos, cap, virt_bits); buf_append(out, &pos, cap, " bits virtual\n");
        buf_append(out, &pos, cap, "power management:\n");
        buf_append(out, &pos, cap, "\n");
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

size_t get_procfs_content(const proc_node_t *node, char *out) {
    switch (node->entry) {
        case PROC_FILE_MAPS:    return build_maps(node->pid, out);
        case PROC_FILE_MOUNTS:  return build_mounts(out);
        case PROC_FILE_AUXV:    return build_auxv(node->pid, out);
        case PROC_FILE_CPUINFO: return build_cpuinfo(out);
        default:                return 0;
    }
}

static void write_name(char *name, size_t name_size, const char *src) {
    strncpy(name, src, name_size - 1);
    name[name_size - 1] = '\0';
}

bool is_procfs_path(const char *abs_path) {
    if (!abs_path) return false;
    if (strcmp(abs_path, "/proc") == 0) return true;
    return starts_with(abs_path, "/proc/");
}

static bool procfs_resolve_impl(const char *abs_path, const char *orig_path, int self, proc_node_t *out, bool follow_self) {
    out->type   = PROC_NODE_NONE;
    out->entry  = PROC_NONE;
    out->pid    = -1;
    out->fd_num = -1;

    if (!starts_with(abs_path, "/proc")) return false;

    // The part after "/proc" (may be empty string for "/proc" itself).
    const char *rel = abs_path + 5;
    if (strcmp(rel, "/") == 0) rel = "";  // treat "/proc/" same as "/proc"

    // build_abs_path_at() normalizes trailing slashes away, so use the original
    // user path as a hint that /proc/self/ must follow the link even for lstat().
    const char *orig_rel = (orig_path && starts_with(orig_path, "/proc")) ? orig_path + 5 : rel;
    char resolved[256];
    if (starts_with(orig_rel, "/self/") || starts_with(rel, "/self/") ||
        (follow_self && strcmp(rel, "/self") == 0)) {
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

bool resolve_procfs(const char *abs_path, int self, proc_node_t *out) {
    return procfs_resolve_impl(abs_path, NULL, self, out, true);
}

bool resolve_procfs_nofollow(const char *abs_path, int self, proc_node_t *out) {
    return procfs_resolve_impl(abs_path, NULL, self, out, false);
}

bool resolve_procfs_nofollow_orig(const char *abs_path, const char *orig_path, int self, proc_node_t *out) {
    return procfs_resolve_impl(abs_path, orig_path, self, out, false);
}

bool is_procfs_dir(const proc_node_t *node) {
    return node->type == PROC_NODE_DIR;
}

int read_procfs_link(const proc_node_t *node, int self, char *out, size_t out_size) {
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

bool get_procfs_dirent(const proc_node_t *dir, int self, int index, char *name, size_t name_size, uint8_t *type_out) {
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
