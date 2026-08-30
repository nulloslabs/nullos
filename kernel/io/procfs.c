#include <stdint.h>
#include <stdbool.h>
#include <cpuid.h>
#include <dirent.h>
#include <main/string.h>
#include <main/sched.h>
#include <main/fd.h>
#include <main/mp.h>
#include <main/machine_info.h>
#include <main/timekeeping.h>
#include <io/devtmpfs.h>
#include <io/time.h>
#include <io/procfs.h>
#include <io/vfs.h>
#include <mm/pmm.h>
#include <mm/vma.h>

const proc_static_node_t proc_nodes[] = {
    { "",              PROC_NODE_DIR,     PROC_DIR_ROOT         },
    { "/self",         PROC_NODE_SYMLINK, PROC_LINK_SELF        },
    { "/mounts",       PROC_NODE_SYMLINK, PROC_LINK_ROOT_MOUNTS },
    { "/cpuinfo",      PROC_NODE_FILE,    PROC_FILE_CPUINFO     },
    { "/meminfo",      PROC_NODE_FILE,    PROC_FILE_MEMINFO     },
    { "/uptime",       PROC_NODE_FILE,    PROC_FILE_UPTIME      },
    { "/stat",         PROC_NODE_FILE,    PROC_FILE_ROOT_STAT   },
    { "/loadavg",      PROC_NODE_FILE,    PROC_FILE_LOADAVG     },
    { "/<pid>",        PROC_NODE_DIR,     PROC_DIR_PID          },
    { "/<pid>/fd",     PROC_NODE_DIR,     PROC_DIR_FD           },
    { "/<pid>/maps",   PROC_NODE_FILE,    PROC_FILE_MAPS        },
    { "/<pid>/mounts", PROC_NODE_FILE,    PROC_FILE_MOUNTS      },
    { "/<pid>/auxv",   PROC_NODE_FILE,    PROC_FILE_AUXV        },
    { "/<pid>/stat",   PROC_NODE_FILE,    PROC_FILE_STAT        },
    { "/<pid>/status", PROC_NODE_FILE,    PROC_FILE_STATUS      },
    { "/<pid>/cmdline",PROC_NODE_FILE,    PROC_FILE_CMDLINE     },
    { "/<pid>/comm",   PROC_NODE_FILE,    PROC_FILE_COMM        },
    { "/<pid>/exe",    PROC_NODE_SYMLINK, PROC_LINK_EXE         },
    { "/<pid>/cwd",    PROC_NODE_SYMLINK, PROC_LINK_CWD         },
    { "/<pid>/fd/<n>", PROC_NODE_SYMLINK, PROC_LINK_FD          },
};

const dirent_static_t root_children[] = {
    { "self",    DT_LNK },
    { "mounts",  DT_LNK },
    { "cpuinfo", DT_REG },
    { "meminfo", DT_REG },
    { "uptime",  DT_REG },
    { "stat",    DT_REG },
    { "loadavg", DT_REG },
};

const dirent_static_t pid_children[] = {
    { "fd",     DT_DIR },
    { "maps",   DT_REG },
    { "mounts", DT_REG },
    { "auxv",   DT_REG },
    { "stat",   DT_REG },
    { "status", DT_REG },
    { "cmdline",DT_REG },
    { "comm",   DT_REG },
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

static int proc_task_index_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->state != TASK_ZOMBIE && tasks[i]->pid == pid) return i;
    }
    return -1;
}

static int nth_open_fd(int pid_idx, int n) {
    int count = 0;
    for (int fd = 0; fd < FD_MAX; fd++) {
        fd_entry_t *e = tasks[pid_idx]->fd_table.entries[fd];
        if (e && e->open) {
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
                *pid_idx = proc_task_index_by_pid(raw_pid);
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
    int words = tasks[pid_idx]->auxv_blob_words;
    if (words <= 0) return 0;
    size_t size = (size_t)words * sizeof(uint64_t);
    if (size > PROCFS_MAX_CONTENT) size = PROCFS_MAX_CONTENT;
    memcpy(out, tasks[pid_idx]->auxv_blob, size);
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
        if (list_vfs_mount(i, line, sizeof(line)) <= 0) break;
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

static void buf_append_u64(char *buf, size_t *pos, size_t cap, uint64_t v) {
    char tmp[32]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    char fwd[32];
    for (int k = 0; k < n; k++) fwd[k] = tmp[n - 1 - k];
    fwd[n] = '\0';
    buf_append(buf, pos, cap, fwd);
}

static void append_meminfo_kb(char *out, size_t *pos, const char *name, uint64_t bytes) {
    buf_append(out, pos, PROCFS_MAX_CONTENT, name);
    buf_append(out, pos, PROCFS_MAX_CONTENT, ": ");
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, bytes / 1024ULL);
    buf_append(out, pos, PROCFS_MAX_CONTENT, " kB\n");
}

static size_t build_meminfo(char *out) {
    size_t pos = 0;
    uint64_t total = get_total_pmm_memory();
    uint64_t free = get_free_pmm_memory();
    uint64_t used = total > free ? total - free : 0;
    uint64_t buffers = used / 8;
    uint64_t cached = used / 4;
    uint64_t available = free + buffers + cached;
    if (available > total) available = total;
    out[0] = '\0';
    append_meminfo_kb(out, &pos, "MemTotal", total);
    append_meminfo_kb(out, &pos, "MemFree", free);
    append_meminfo_kb(out, &pos, "MemAvailable", available);
    append_meminfo_kb(out, &pos, "Buffers", buffers);
    append_meminfo_kb(out, &pos, "Cached", cached);
    append_meminfo_kb(out, &pos, "SwapTotal", 0);
    append_meminfo_kb(out, &pos, "SwapFree", 0);
    return pos;
}

static size_t build_uptime(char *out) {
    uint64_t uptime = get_monotonic_time_us();
    uint64_t idle = get_idle_time_us();
    size_t pos = 0;
    out[0] = '\0';
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, uptime / 1000000ULL);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, ".");
    uint64_t uptime_hundredths = (uptime / 10000ULL) % 100ULL;
    if (uptime_hundredths < 10) buf_append(out, &pos, PROCFS_MAX_CONTENT, "0");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, uptime_hundredths);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, idle / 1000000ULL);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, ".");
    uint64_t idle_hundredths = (idle / 10000ULL) % 100ULL;
    if (idle_hundredths < 10) buf_append(out, &pos, PROCFS_MAX_CONTENT, "0");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, idle_hundredths);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\n");
    return pos;
}

static void append_cpu_stat(char *out, size_t *pos, const char *name, uint64_t user, uint64_t system, uint64_t idle) {
    buf_append(out, pos, PROCFS_MAX_CONTENT, name);
    buf_append(out, pos, PROCFS_MAX_CONTENT, "  ");
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, user);
    buf_append(out, pos, PROCFS_MAX_CONTENT, " 0 ");
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, system);
    buf_append(out, pos, PROCFS_MAX_CONTENT, " ");
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, idle);
    buf_append(out, pos, PROCFS_MAX_CONTENT, " 0 0 0 0 0 0\n");
}

static size_t build_root_stat(char *out) {
    uint64_t uptime_us = get_monotonic_time_us();
    uint64_t bsp_idle_us = get_idle_time_us();
    if (bsp_idle_us > uptime_us) bsp_idle_us = uptime_us;
    uint64_t bsp_user = (uptime_us - bsp_idle_us) / 10000ULL;
    uint64_t bsp_idle = bsp_idle_us / 10000ULL;
    uint64_t other_idle = cpu_count > 1 ? (uint64_t)(cpu_count - 1) * (uptime_us / 10000ULL) : 0;
    uint64_t timer_interrupts = get_timer_interrupt_count();
    uint64_t realtime = time_get_realtime_us();
    uint64_t boot_time = realtime > uptime_us ? (realtime - uptime_us) / 1000000ULL : 0;
    size_t pos = 0;
    out[0] = '\0';

    append_cpu_stat(out, &pos, "cpu", bsp_user, 0, bsp_idle + other_idle);
    append_cpu_stat(out, &pos, "cpu0", bsp_user, 0, bsp_idle);
    for (int i = 1; i < cpu_count; i++) {
        char name[16] = "cpu";
        fmt_int(i, name + 3, sizeof(name) - 3);
        append_cpu_stat(out, &pos, name, 0, 0, uptime_us / 10000ULL);
    }
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "intr ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, timer_interrupts);
    for (int i = 0; i < 32; i++) buf_append(out, &pos, PROCFS_MAX_CONTENT, " 0");
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, timer_interrupts);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nctxt ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, get_context_switch_count());
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nbtime ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, boot_time);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nprocesses ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, get_processes_created());
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nprocs_running ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, get_runnable_task_count());
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nprocs_blocked 0\nsoftirq 0\n");
    return pos;
}

static void append_loadavg(char *out, size_t *pos, unsigned long load) {
    uint64_t hundredths = ((uint64_t)load * 100ULL + 32768ULL) / 65536ULL;
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, hundredths / 100ULL);
    buf_append(out, pos, PROCFS_MAX_CONTENT, ".");
    if (hundredths % 100ULL < 10) buf_append(out, pos, PROCFS_MAX_CONTENT, "0");
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, hundredths % 100ULL);
}

static size_t build_loadavg(char *out) {
    unsigned long loads[3];
    get_load_averages(loads);
    uint64_t total = get_process_count();
    if (total) total--;
    size_t pos = 0;
    out[0] = '\0';
    append_loadavg(out, &pos, loads[0]);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
    append_loadavg(out, &pos, loads[1]);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
    append_loadavg(out, &pos, loads[2]);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, get_runnable_task_count());
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "/");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, total);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " ");
    buf_append_u64(out, &pos, PROCFS_MAX_CONTENT, get_last_created_pid());
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\n");
    return pos;
}

static char get_task_state(const task_t *task) {
    if (task->state == TASK_ZOMBIE) return 'Z';
    if (task->state == TASK_SLEEPING) return 'S';
    if (task->state == TASK_STOPPED) return task->stopped_by_signal ? 'T' : 'S';
    return 'R';
}

static int count_task_threads(int pid_idx) {
    int count = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i]->state != TASK_DEAD && tasks[i]->state != TASK_ZOMBIE && tasks[i]->ctx == tasks[pid_idx]->ctx) count++;
    return count ? count : 1;
}

static void append_stat_value(char *out, size_t *pos, uint64_t value) {
    buf_append(out, pos, PROCFS_MAX_CONTENT, " ");
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, value);
}

static void append_stat_signed_value(char *out, size_t *pos, int64_t value) {
    buf_append(out, pos, PROCFS_MAX_CONTENT, " ");
    if (value < 0) { buf_append(out, pos, PROCFS_MAX_CONTENT, "-"); value = -value; }
    buf_append_u64(out, pos, PROCFS_MAX_CONTENT, (uint64_t)value);
}

static size_t build_stat(int pid_idx, char *out) {
    const task_t *task = tasks[pid_idx];
    size_t pos = 0; out[0] = '\0';
    buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->pid);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, " (");
    buf_append(out, &pos, PROCFS_MAX_CONTENT, task->name[0] ? task->name : task->exe);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, ") ");
    char state[2] = { get_task_state(task), '\0' };
    buf_append(out, &pos, PROCFS_MAX_CONTENT, state);
    append_stat_value(out, &pos, task->ppid);
    append_stat_value(out, &pos, task->pgid);
    append_stat_value(out, &pos, task->sid);
    append_stat_value(out, &pos, 0);
    append_stat_value(out, &pos, task->pgid);
    for (int field = 9; field <= 17; field++) append_stat_value(out, &pos, 0);
    append_stat_value(out, &pos, 20 + task->nice);
    append_stat_signed_value(out, &pos, task->nice);
    append_stat_value(out, &pos, count_task_threads(pid_idx));
    for (int field = 21; field <= 52; field++) append_stat_value(out, &pos, field == 47 ? task->brk_start : 0);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\n");
    return pos;
}

static size_t build_status(int pid_idx, char *out) {
    const task_t *task = tasks[pid_idx];
    size_t pos = 0; out[0] = '\0';
    char state[2] = { get_task_state(task), '\0' };
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "Name:\t"); buf_append(out, &pos, PROCFS_MAX_CONTENT, task->name[0] ? task->name : task->exe); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nState:\t"); buf_append(out, &pos, PROCFS_MAX_CONTENT, state); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nTgid:\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->pid);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nPid:\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->pid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nPPid:\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->ppid);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nUid:\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->uid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->euid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->euid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->fsuid);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nGid:\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->gid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->egid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->egid); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, task->fsgid);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nThreads:\t"); buf_append_uint(out, &pos, PROCFS_MAX_CONTENT, count_task_threads(pid_idx)); buf_append(out, &pos, PROCFS_MAX_CONTENT, "\nVmSize:\t0 kB\nVmRSS:\t0 kB\n");
    return pos;
}

static size_t build_cmdline(int pid_idx, char *out) {
    const char *cmdline = tasks[pid_idx]->exe[0] ? tasks[pid_idx]->exe : tasks[pid_idx]->name;
    size_t length = strlen(cmdline);
    if (length >= PROCFS_MAX_CONTENT) length = PROCFS_MAX_CONTENT - 1;
    memcpy(out, cmdline, length);
    out[length] = '\0';
    return length + 1;
}

static size_t build_comm(int pid_idx, char *out) {
    size_t pos = 0; out[0] = '\0';
    buf_append(out, &pos, PROCFS_MAX_CONTENT, tasks[pid_idx]->name[0] ? tasks[pid_idx]->name : tasks[pid_idx]->exe);
    buf_append(out, &pos, PROCFS_MAX_CONTENT, "\n");
    return pos;
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
        case PROC_FILE_MEMINFO: return build_meminfo(out);
        case PROC_FILE_UPTIME:  return build_uptime(out);
        case PROC_FILE_ROOT_STAT: return build_root_stat(out);
        case PROC_FILE_LOADAVG: return build_loadavg(out);
        case PROC_FILE_STAT:    return build_stat(node->pid, out);
        case PROC_FILE_STATUS:  return build_status(node->pid, out);
        case PROC_FILE_CMDLINE: return build_cmdline(node->pid, out);
        case PROC_FILE_COMM:    return build_comm(node->pid, out);
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

    // Strip trailing slashes from rel into a cleaned copy so pattern matching
    // works for paths like "/proc/self/", "/proc/1/", "/proc/1/fd/". Without
    // this, the trailing '/' breaks match_pattern's end-of-string check.
    char rel_clean[256];
    {
        size_t rlen = strlen(rel);
        if (rlen > 1 && rel[rlen - 1] == '/') {
            strncpy(rel_clean, rel, sizeof(rel_clean) - 1);
            rel_clean[sizeof(rel_clean) - 1] = '\0';
            while (rlen > 1 && rel_clean[rlen - 1] == '/') rel_clean[--rlen] = '\0';
            rel = rel_clean;
        }
    }

    // build_abs_path_at() normalizes trailing slashes away, so use the original
    // user path as a hint that /proc/self/ must follow the link even for lstat().
    const char *orig_rel = (orig_path && starts_with(orig_path, "/proc")) ? orig_path + 5 : rel;
    char resolved[256];
    if (starts_with(orig_rel, "/self/") || starts_with(rel, "/self/") ||
        (follow_self && strcmp(rel, "/self") == 0)) {
        char pidbuf[16];
        fmt_int(tasks[self]->pid, pidbuf, sizeof(pidbuf));
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
            if (tasks[pid_idx]->state == TASK_DEAD || tasks[pid_idx]->state == TASK_ZOMBIE) return false;
            if (!current_task_ptr) return false;
            if (current_task_ptr->euid != 0 && current_task_ptr->euid != tasks[pid_idx]->euid) return false;
        }

        // Validate fd if one was parsed.
        if (fd_num >= 0) {
            if (fd_num >= FD_MAX) return false;
            fd_entry_t *e = tasks[pid_idx]->fd_table.entries[fd_num];
            if (!e || !e->open) return false;
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
        return fmt_int(tasks[self]->pid, out, out_size);
    case PROC_LINK_ROOT_MOUNTS:
        return copy_str("self/mounts", out, out_size);
    case PROC_LINK_EXE:
        return copy_str(tasks[node->pid]->exe, out, out_size);
    case PROC_LINK_CWD:
        return copy_str(tasks[node->pid]->cwd, out, out_size);
    case PROC_LINK_FD: {
        fd_entry_t *e = tasks[node->pid]->fd_table.entries[node->fd_num];
        if (!e || !e->open) return -1;
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
            if (tasks[i]->state == TASK_DEAD || tasks[i]->state == TASK_ZOMBIE) continue;
            if (tasks[i]->pid == 0) continue;
            if (count++ == target) {
                fmt_int(tasks[i]->pid, name, name_size);
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
