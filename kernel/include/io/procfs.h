#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/sys/stat.h>

#define PROCFS_MAX_CONTENT 4096
#define PROC_NODE_COUNT (int)(sizeof(proc_nodes) / sizeof(proc_nodes[0]))

typedef enum {
    PROC_NODE_NONE = 0,
    PROC_NODE_DIR,
    PROC_NODE_FILE,
    PROC_NODE_SYMLINK,
} proc_node_type_t;

typedef enum {
    PROC_NONE = 0,
    PROC_DIR_ROOT,
    PROC_DIR_PID,
    PROC_DIR_FD,
    PROC_FILE_MAPS,
    PROC_FILE_MOUNTS,
    PROC_FILE_AUXV,
    PROC_LINK_SELF,
    PROC_LINK_ROOT_MOUNTS,
    PROC_LINK_EXE,
    PROC_LINK_CWD,
    PROC_LINK_FD,
} proc_entry_t;

typedef struct {
    proc_node_type_t type;
    proc_entry_t entry;
    int pid;
    int fd_num;
} proc_node_t;

typedef struct {
    const char *pattern;
    proc_node_type_t type;
    proc_entry_t entry;
} proc_static_node_t;

typedef struct {
    const char *name;
    uint8_t dt_type;
} dirent_static_t;

extern const proc_static_node_t proc_nodes[];
extern const dirent_static_t root_children[];
extern const dirent_static_t pid_children[];

bool resolve_procfs(const char *abs_path, int self, proc_node_t *out);
bool resolve_procfs_nofollow(const char *abs_path, int self, proc_node_t *out);
bool resolve_procfs_nofollow_orig(const char *abs_path, const char *orig_path, int self, proc_node_t *out);
bool is_procfs_path(const char *abs_path);
bool is_procfs_dir(const proc_node_t *node);
bool get_procfs_dirent(const proc_node_t *dir, int self, int index, char *name, size_t name_size, uint8_t *type_out);
size_t get_procfs_content(const proc_node_t *node, char *out);
int read_procfs_link(const proc_node_t *node, int self, char *out, size_t out_size);
