#include <freestanding/stdint.h>
#include <main/fd.h>
#include <main/string.h>
#include <main/errno.h>
#include <main/scheduler.h>

int alloc_fd(fd_table_t *table, const char *path, fd_type_t type, uint32_t flags) {
    for (int i = 0; i < FD_MAX; i++) {
        if (!table->entries[i].open) {
            table->entries[i].open   = true;
            table->entries[i].type   = type;
            table->entries[i].offset = 0;
            table->entries[i].flags  = flags;
            strncpy(table->entries[i].path, path, 255);
            table->entries[i].path[255] = '\0';
            return i;
        }
    }
    errno = EMFILE;
    return -EMFILE;
}

int free_fd(fd_table_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX) { errno = EBADF; return -EBADF; }
    if (!table->entries[fd].open) { errno = EBADF; return -EBADF; }
    table->entries[fd].open   = false;
    table->entries[fd].type   = FD_NONE;
    table->entries[fd].offset = 0;
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

void init_fd_table(fd_table_t *table) {
    for (int i = 0; i < FD_MAX; i++) {
        // Clear EVERYTHING first
        table->entries[i].open = false;
        table->entries[i].type = FD_NONE;
        table->entries[i].offset = 0;
        table->entries[i].flags = 0;
        table->entries[i].path[0] = '\0';
    }

    alloc_fd(table, "stdin",  FD_STREAM, 0); // Becomes FD 0
    alloc_fd(table, "stdout", FD_STREAM, 0); // Becomes FD 1
    alloc_fd(table, "stderr", FD_STREAM, 0); // Becomes FD 2
}