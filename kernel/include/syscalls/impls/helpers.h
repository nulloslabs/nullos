#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/statx.h>
#include <io/initrd.h>

#define USER_ADDR_MAX 0x0000800000000000ULL
#define MAX_BRK_SIZE (256ULL * 1024ULL * 1024ULL)
#define MAX_IOV 1024
#define MAX_IO_COUNT (16 * 1024 * 1024)
#define MAX_FUTEX_WAITERS 256
#define SHMEM_BOGO_DIRENT_SIZE 20

#define FW_FREE       0
#define FW_WAITING    1
#define FW_WOKEN      2
#define FW_TIMED_OUT  3

#define FD_SETSIZE 1024
#define FD_SET_BYTES (FD_SETSIZE / 8)

typedef struct {
    int      state;
    uint64_t phys_addr;
    int      task_idx;
    uint32_t bitset;
    uint64_t deadline_us;
} futex_waiter_t;

typedef struct {
    syscall_frame_t context;
    uint64_t blocked_signals;
} __attribute__((packed)) signal_stack_frame_t;

extern futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];
extern spinlock_t stdin_lock;
extern spinlock_t futex_lock;

bool user_address_range_ok(uint64_t addr, uint64_t size);
bool user_range_ok(vmm_context_t *ctx, uint64_t addr, uint64_t size);
bool user_write_range_ok(vmm_context_t *ctx, uint64_t addr, uint64_t size);
bool user_page_range_ok(uint64_t addr, uint64_t size, uint64_t *start, uint64_t *end);
bool fd_allows_read(const fd_entry_t *entry);
bool fd_allows_write(const fd_entry_t *entry);
mode_t apply_current_umask(mode_t mode);
bool user_range_is_mapped(vmm_context_t *ctx, uint64_t addr, uint64_t size);
bool can_access_initrd(const initrd_file_t *file, int want_read, int want_write, int want_exec);
bool can_access_stat_mode(const struct stat *st, int want_read, int want_write, int want_exec);
int proc_self_idx(void);
void resolve_path_symlinks_ex(const char *path, char *out, size_t out_size, bool follow_final);
void resolve_path_symlinks(const char *path, char *out, size_t out_size);
int build_abs_path_at(int dirfd, const char *path, char *out, size_t out_size);
void build_abs_path(const char *path, char *out, size_t out_size);
int copy_string_from_user(char *dest, const char *src, size_t capacity);
int copy_from_user_strarray(char ***out_karray, const char **user_arr, size_t max_elements);
int ptm_path_idx(const char *path);
void free_strarray(char **arr, int count);
bool stat_virtual_device(const char *abs_path, struct stat *kst);
bool stat_tmpfs_to_kst(const char *abs_path, struct stat *kst, bool follow);
bool stat_initrd_to_kst(const char *abs_path, struct stat *kst, bool follow);
bool stat_ext4_to_kst(const char *abs_path, struct stat *kst, bool follow);
bool stat_iso9660_to_kst(const char *abs_path, struct stat *kst, bool follow);
bool stat_vfat_to_kst(const char *abs_path, struct stat *kst, bool follow);
int check_parent_access(const char *path, bool modify);
bool stat_proc(const char *abs_path, const char *orig_path, struct stat *kst, bool follow_self);
int proc_open_common(char *abs_path, size_t abs_size, uint32_t flags);
int open_tmpfs_common(const char *abs_path, uint32_t flags, mode_t mode);
int open_ext4_common(const char *abs_path, uint32_t flags);
int open_iso9660_common(const char *abs_path, uint32_t flags);
int open_fat32_common(const char *abs_path, uint32_t flags);
uint64_t resolve_futex_key(uint32_t *uaddr, syscall_frame_t *frame);
void wait_futex(syscall_frame_t *frame, uint64_t phys, uint32_t val, struct timespec *timeout_ptr, uint32_t bitset, bool absolute_timeout);
int wake_futex(uint64_t phys, uint32_t max_wake, uint32_t bitset);
int fill_rlimit(int resource, rlimit_t *lim);
uint16_t emit_dirent64(uint64_t bufp, uint64_t *written, uint64_t buflen, uint64_t ino, uint64_t off, uint8_t type, const char *name);
uint16_t emit_dirent(uint64_t bufp, uint64_t *written, uint64_t buflen, uint64_t ino, uint64_t off, uint8_t type, const char *name);
void resolve_dir_for_readdir(const char *fd_path, char *prefix_out, size_t prefix_size, char *abs_out, size_t abs_size);
int tty_rel_to_idx(const char *rel);
int pty_rel_to_idx(const char *rel);
int ioctl_tty_idx(fd_entry_t *entry);
task_t *find_priority_task(int which, id_t who);
int prepare_unix_socket_path(uint8_t *addr, uint32_t *addrlen, bool binding);
int reject_procfs_mutation(const char *path);
int reject_virtual_removal(const char *path);
int change_path_ownership(const char *path, uid_t uid, gid_t gid, bool follow);
int set_path_times(const char *path, struct timespec atime, bool set_atime, struct timespec mtime, bool set_mtime, bool follow);
int get_utimens_times(const struct timespec *user_times, struct timespec *atime, bool *set_atime, struct timespec *mtime, bool *set_mtime);
int epoll_collect(epoll_instance_t *epi, struct epoll_event *out, int maxevents);
int epoll_find_interest(epoll_instance_t *epi, int fd);
int64_t do_select(int nfds, uint8_t *k_read, uint8_t *k_write, uint8_t *k_except, uint8_t *out_read, uint8_t *out_write, uint8_t *out_except, int out_bytes, int64_t timeout_us);
uint64_t do_read(int fd, void *buf, size_t count);
uint64_t do_write(int fd, const void *buf, size_t count);
int do_clock_nanosleep(int clock_id, int flags, const struct timespec *req, struct timespec *rem);
int do_epoll_create1(int flags);
int do_epoll_wait(syscall_frame_t *frame, int64_t timeout_us, uint64_t sigmask_arg);
int timeval_to_us(const struct timeval *tv, uint64_t *out);
int timespec_to_us(const struct timespec *ts, int64_t *out);
void fill_real_itimer(task_t *task, struct itimerval *value);
int copy_from_user(void *kdest, const void *usrc, size_t size);
int copy_to_user(const void *udest, const void *ksrc, size_t size);
void check_signals(syscall_frame_t *frame);
void check_signals_from_user_exception(syscall_frame_t *frame);
void check_futex_timeouts(void);
void cleanup_futex_task(int task_idx);
void process_robust_list(task_t *task);
void wake_clear_child_tid(task_t *task);
void stat_to_statx(struct statx *sx, const struct stat *kst, const char *abs_path);
void statx_add_fs_metadata(struct statx *sx, const char *path, bool follow, unsigned int mask);
int stat_fd_to_kst(int fd, struct stat *kst);
void rollback_mmap(void *ptr, uint64_t num_pages, uint64_t retained_pages);
flock_obj_t *find_advisory_lock_conflict(const fd_entry_t *entry, int requested_type, bool process_lock);
int set_advisory_lock(fd_entry_t *entry, int lock_type, bool nonblocking, bool process_lock);
int do_truncate_path(const char *abs_path, uint64_t length);
int change_working_directory(const char *abs_path);
int check_access_at(int dirfd, const char *user_path, int mode, int flags);
void wake_clear_child_tid(task_t *task);
void check_signals_from_user_exception(syscall_frame_t *frame);
void cleanup_futex_task(int task_idx);
void process_robust_list(task_t *task);
