#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <main/assert.h>
#include <main/fd.h>
#include <main/string.h>
#include <main/sched.h>
#include <io/pty.h>
#include <io/sockets.h>
#include <io/unix_sockets.h>
#include <mm/mm.h>

flock_obj_t global_flocks[128];

flock_obj_t *alloc_flock_obj(const char *path) {
    for (int i = 0; i < 128; i++) {
        if (!global_flocks[i].used) {
            global_flocks[i].used = true;
            global_flocks[i].refcount = 1;
            global_flocks[i].lock_type = 0;
            global_flocks[i].owner_pid = 0;
            strncpy(global_flocks[i].path, path, 255);
            global_flocks[i].path[255] = '\0';
            return &global_flocks[i];
        }
    }
    return NULL;
}

void retain_flock_obj(flock_obj_t *obj) {
    if (obj) obj->refcount++;
}

void release_flock_obj(flock_obj_t *obj) {
    if (obj) {
        obj->refcount--;
        if (obj->refcount <= 0) {
            obj->used = false;
            obj->lock_type = 0;
            obj->owner_pid = 0;
        }
    }
}

int alloc_fd_handle(fd_table_t *table, const char *path, fd_type_t type, uint32_t flags, void *handle) {
    // respect RLIMIT_NOFILE soft limit (configurable via setrlimit/prlimit)
    uint64_t limit = FD_MAX;
    if (current_task_ptr) limit = current_task_ptr->rlimit_nofile.rlim_cur;
    if (limit > FD_MAX) limit = FD_MAX;
    for (int i = 0; i < (int)limit; i++) {
        if (!table->entries[i]) {
            fd_entry_t *e = malloc(sizeof(fd_entry_t));
            if (!e) return -ENOMEM;
            e->open   = true;
            e->type   = type;
            e->offset = 0;
            e->flags  = flags;
            e->handle = handle;
            strncpy(e->path, path, 255);
            e->path[255] = '\0';
            table->entries[i] = e;
            return i;
        }
    }
    return -EMFILE;
}

int alloc_fd(fd_table_t *table, const char *path, fd_type_t type, uint32_t flags) { return alloc_fd_handle(table, path, type, flags, NULL); }

int free_fd(fd_table_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX) { return -EBADF; }
    fd_entry_t *e = table->entries[fd];
    if (!e || !e->open) { return -EBADF; }
    if (e->type == FD_PTY_MASTER) {
        int idx = -1;
        if (strncmp(e->path, "ptm:", 4) == 0) {
            const char *p = e->path + 4;
            idx = 0;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
        }
        release_pty_master(idx);
    } else if (e->type == FD_DEV) {
        int idx = pty_slave_path_idx(e->path);
        if (idx >= 0)
            release_pty_slave(idx);
    } else if (e->type == FD_PIPE) {
        release_unix_handle((unix_handle_t *)e->handle);
    } else if (e->type == FD_SOCKET) {
        release_socket((socket_t *)e->handle);
    } else if ((e->type == FD_FILE || e->type == FD_TMPFS) && e->handle != NULL) {
        release_flock_obj((flock_obj_t *)e->handle);
    } else if (e->type == FD_EPOLL) {
        epoll_instance_t *epi = (epoll_instance_t *)e->handle;
        if (epi && --epi->refcount == 0) free(epi);
    }
    free(e);
    table->entries[fd] = NULL;
    return 0;
}

fd_entry_t *get_fd(fd_table_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX) return NULL;
    fd_entry_t *e = table->entries[fd];
    if (!e || !e->open) return NULL;
    return e;
}

fd_entry_t *get_current_fd(int fd) {
    if (fd < 0 || fd >= FD_MAX) return NULL;
    fd_entry_t* entry = current_task_ptr->fd_table.entries[fd];
    return (entry && entry->open) ? entry : NULL;
}

void retain_fd_entry(fd_entry_t *entry) {
    if (!entry || !entry->open) return;
    assert(entry->type != FD_NONE);
    if (entry->type == FD_PTY_MASTER) {
        int idx = -1;
        if (strncmp(entry->path, "ptm:", 4) == 0) {
            const char *p = entry->path + 4;
            idx = 0;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
        }
        retain_pty_master(idx);
    } else if (entry->type == FD_DEV) {
        int idx = pty_slave_path_idx(entry->path);
        if (idx >= 0)
            retain_pty_slave(idx);
    } else if (entry->type == FD_PIPE) {
        retain_unix_handle((unix_handle_t *)entry->handle);
    } else if (entry->type == FD_SOCKET) {
        retain_socket((socket_t *)entry->handle);
    } else if (entry->type == FD_EPOLL) {
        epoll_instance_t *epi = (epoll_instance_t *)entry->handle;
        if (epi) epi->refcount++;
    } else if ((entry->type == FD_FILE || entry->type == FD_TMPFS) && entry->handle != NULL) { retain_flock_obj((flock_obj_t *)entry->handle); } }

void init_fd_table(fd_table_t *table) {
    for (int i = 0; i < FD_MAX; i++) {
        table->entries[i] = NULL;
    }

    alloc_fd(table, "stdin",  FD_STREAM, O_RDONLY); // Becomes FD 0
    alloc_fd(table, "stdout", FD_STREAM, O_WRONLY); // Becomes FD 1
    alloc_fd(table, "stderr", FD_STREAM, O_WRONLY); // Becomes FD 2
}
