#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <errno.h>
#include <asm/unistd.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/futex.h>
#include <sys/reboot.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <main/log.h>
#include <main/spinlocks.h>
#include <main/halt.h>
#include <main/hostname.h>
#include <main/domainname.h>
#include <main/utsname.h>
#include <main/msr.h>
#include <main/sched.h>
#include <main/string.h>
#include <main/rng.h>
#include <io/fonts.h>
#include <io/devtmpfs.h>
#include <io/pts_devices.h>
#include <io/terminal.h>
#include <io/keyboard.h>
#include <io/pty.h>
#include <io/time.h>
#include <io/power.h>
#include <io/net.h>
#include <io/serial.h>
#include <io/tmpfs.h>
#include <io/iso9660.h>
#include <io/mbr.h>
#include <io/usb.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/sys.h>

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

void sys_setrlimit(syscall_frame_t *frame) {
    int resource = (int)frame->rdi;
    rlimit_t *rlim = (rlimit_t *)frame->rsi;
    rlimit_t current;

    if (!rlim) { frame->rax = (uint64_t)-EFAULT; return; }
    int ret = fill_rlimit(resource, &current);
    if (ret < 0) { frame->rax = (uint64_t)ret; return; }

    rlimit_t requested;
    if (copy_from_user(&requested, rlim, sizeof(requested)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (resource == RLIMIT_NOFILE) {
        if (requested.rlim_cur > requested.rlim_max) { frame->rax = (uint64_t)-EINVAL; return; }
        if (requested.rlim_max > FD_MAX) { frame->rax = (uint64_t)-EPERM; return; }
        if (requested.rlim_max > current.rlim_max && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
        if (requested.rlim_cur > requested.rlim_max) { frame->rax = (uint64_t)-EINVAL; return; }
        current_task_ptr->rlimit_nofile = requested;
        frame->rax = 0;
        return;
    }
    if (requested.rlim_cur != current.rlim_cur || requested.rlim_max != current.rlim_max) { frame->rax = (uint64_t)-EPERM; return; }

    frame->rax = 0;
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
        if (resource == RLIMIT_NOFILE) {
            if (requested.rlim_cur > requested.rlim_max) { frame->rax = (uint64_t)-EINVAL; return; }
            if (requested.rlim_max > FD_MAX) { frame->rax = (uint64_t)-EPERM; return; }
            if (requested.rlim_max > current.rlim_max && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
            current_task_ptr->rlimit_nofile = requested;
        } else {
            if (requested.rlim_cur != current.rlim_cur || requested.rlim_max != current.rlim_max) {
                frame->rax = (uint64_t)-EPERM;
                return;
            }
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
