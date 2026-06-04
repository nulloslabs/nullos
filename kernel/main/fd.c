#include <freestanding/stdint.h>
#include <main/fd.h>
#include <main/string.h>
#include <main/errno.h>
#include <main/scheduler.h>
#include <io/pty.h>
#include <io/sockets.h>

int alloc_fd_handle(fd_table_t *table, const char *path, fd_type_t type, uint32_t flags, void *handle) {
    for (int i = 0; i < FD_MAX; i++) {
        if (!table->entries[i].open) {
            table->entries[i].open   = true;
            table->entries[i].type   = type;
            table->entries[i].offset = 0;
            table->entries[i].flags  = flags;
            table->entries[i].handle = handle;
            strncpy(table->entries[i].path, path, 255);
            table->entries[i].path[255] = '\0';
            return i;
        }
    }
    return -EMFILE;
}

int alloc_fd(fd_table_t *table, const char *path, fd_type_t type, uint32_t flags) {
    return alloc_fd_handle(table, path, type, flags, NULL);
}

int free_fd(fd_table_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX) { return -EBADF; }
    if (!table->entries[fd].open) { return -EBADF; }
    if (table->entries[fd].type == FD_PTY_MASTER) {
        int idx = -1;
        if (strncmp(table->entries[fd].path, "ptm:", 4) == 0) {
            const char *p = table->entries[fd].path + 4;
            idx = 0;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + (*p - '0');
                p++;
            }
        }
        release_pty_master(idx);
    } else if (table->entries[fd].type == FD_DEV) {
        int idx = pty_slave_path_idx(table->entries[fd].path);
        if (idx >= 0)
            release_pty_slave(idx);
    } else if (table->entries[fd].type == FD_PIPE || table->entries[fd].type == FD_SOCKET) {
        release_unix_handle((unix_handle_t *)table->entries[fd].handle);
    }
    table->entries[fd].open   = false;
    table->entries[fd].type   = FD_NONE;
    table->entries[fd].offset = 0;
    table->entries[fd].handle = NULL;
    table->entries[fd].path[0] = '\0';
    return 0;
}

fd_entry_t *get_fd(fd_table_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX) return NULL;
    if (!table->entries[fd].open) return NULL;
    return &table->entries[fd];
}

fd_entry_t *get_current_fd(int fd) {
    if (fd < 0 || fd >= FD_MAX) return NULL;
    fd_entry_t* entry = &current_task_ptr->fd_table.entries[fd];
    return entry->open ? entry : NULL;
}

void retain_fd_entry(fd_entry_t *entry) {
    if (!entry || !entry->open) return;
    if (entry->type == FD_PTY_MASTER) {
        int idx = -1;
        if (strncmp(entry->path, "ptm:", 4) == 0) {
            const char *p = entry->path + 4;
            idx = 0;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + (*p - '0');
                p++;
            }
        }
        retain_pty_master(idx);
    } else if (entry->type == FD_DEV) {
        int idx = pty_slave_path_idx(entry->path);
        if (idx >= 0)
            retain_pty_slave(idx);
    } else if (entry->type == FD_PIPE || entry->type == FD_SOCKET) {
        retain_unix_handle((unix_handle_t *)entry->handle);
    }
}

void init_fd_table(fd_table_t *table) {
    for (int i = 0; i < FD_MAX; i++) {
        // Clear EVERYTHING first
        table->entries[i].open = false;
        table->entries[i].type = FD_NONE;
        table->entries[i].offset = 0;
        table->entries[i].flags = 0;
        table->entries[i].handle = NULL;
        table->entries[i].path[0] = '\0';
    }

    alloc_fd(table, "stdin",  FD_STREAM, 0); // Becomes FD 0
    alloc_fd(table, "stdout", FD_STREAM, 0); // Becomes FD 1
    alloc_fd(table, "stderr", FD_STREAM, 0); // Becomes FD 2
}
