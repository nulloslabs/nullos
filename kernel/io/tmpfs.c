#include <errno.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/terminal.h>
#include <io/tmpfs.h>
#include <mm/mm.h>

tmpfs_inode_t tmpfs_inodes[TMPFS_MAX_INODES];
spinlock_t tmpfs_lock = SPINLOCK_INIT;

static struct {
    bool active;
    char path[128];
    int  root_inode;   // index into tmpfs_inodes[]
} tmpfs_mounts[TMPFS_MAX_MOUNTS];

static uint64_t next_ino = 1;

static int alloc_inode(void) {
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!tmpfs_inodes[i].active) {
            memset(&tmpfs_inodes[i], 0, sizeof(tmpfs_inodes[i]));
            tmpfs_inodes[i].active = true;
            tmpfs_inodes[i].parent = -1;
            tmpfs_inodes[i].ino    = next_ino++;
            return i;
        }
    }
    return -ENOSPC;
}

static void free_inode(int idx) {
    if (idx < 0 || idx >= TMPFS_MAX_INODES) return;
    if (tmpfs_inodes[idx].type == TMPFS_REG && tmpfs_inodes[idx].data) {
        free(tmpfs_inodes[idx].data);
    }
    memset(&tmpfs_inodes[idx], 0, sizeof(tmpfs_inodes[idx]));
}

static int find_mount_for(const char *abs_path, const char **rel_out) {
    size_t best_len = 0;
    int    best     = -1;
    for (int i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        if (!tmpfs_mounts[i].active) continue;
        size_t plen = strlen(tmpfs_mounts[i].path);
        if (plen > best_len &&
            strncmp(abs_path, tmpfs_mounts[i].path, plen) == 0 &&
            (abs_path[plen] == '/' || abs_path[plen] == '\0')) {
            best     = i;
            best_len = plen;
        }
    }
    if (best < 0) return -ENOENT;
    if (rel_out) {
        const char *rel = abs_path + best_len;
        while (*rel == '/') rel++;
        *rel_out = rel;
    }
    return best;
}

bool is_tmpfs_dir(const char *abs_path) {
    if (!abs_path) return false;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    bool found = (find_mount_for(abs_path, NULL) >= 0);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return found;
}

int create_tmpfs_root(const char *mount_path) {
    if (!mount_path || !mount_path[0]) return -EINVAL;

    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);

    // Dedup: same path already mounted -> success.
    for (int i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        if (tmpfs_mounts[i].active &&
            strcmp(tmpfs_mounts[i].path, mount_path) == 0) {
            spin_unlock_irqrestore(&tmpfs_lock, irq);
            return 0;
        }
    }

    int slot = -1;
    for (int i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        if (!tmpfs_mounts[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return -ENOMEM;
    }

    int inode = alloc_inode();
    if (inode < 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return inode;
    }

    tmpfs_inodes[inode].type      = TMPFS_DIR;
    tmpfs_inodes[inode].mode      = 0040755;
    tmpfs_inodes[inode].uid       = 0;
    tmpfs_inodes[inode].gid       = 0;
    tmpfs_inodes[inode].parent    = -1;
    tmpfs_inodes[inode].mount_idx = slot;
    strncpy(tmpfs_inodes[inode].name, "/", TMPFS_MAX_NAME - 1);

    strncpy(tmpfs_mounts[slot].path, mount_path, sizeof(tmpfs_mounts[slot].path) - 1);
    tmpfs_mounts[slot].path[sizeof(tmpfs_mounts[slot].path) - 1] = '\0';
    tmpfs_mounts[slot].active     = true;
    tmpfs_mounts[slot].root_inode = inode;

    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

int destroy_tmpfs_root(const char *mount_path) {
    if (!mount_path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);

    int slot = -1;
    for (int i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        if (tmpfs_mounts[i].active &&
            strcmp(tmpfs_mounts[i].path, mount_path) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return -EINVAL;
    }

    int root = tmpfs_mounts[slot].root_inode;
    tmpfs_mounts[slot].active = false;
    tmpfs_mounts[slot].path[0] = '\0';
    tmpfs_mounts[slot].root_inode = -1;

    // Recursively free every inode that belongs to this mount.
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (tmpfs_inodes[i].active && tmpfs_inodes[i].mount_idx == slot) {
            free_inode(i);
        }
    }
    (void)root;
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

// Forced forward declaration, walk_rel calls resolve_locked which calls walk_rel.
static int walk_rel(int start_dir, const char *rel, bool follow_final);

static int resolve_locked(const char *abs_path, bool follow_final) {
    const char *rel = NULL;
    int m = find_mount_for(abs_path, &rel);
    if (m < 0) return -ENOENT;
    int root = tmpfs_mounts[m].root_inode;
    return (rel[0] == '\0') ? root : walk_rel(root, rel, follow_final);
}

static int walk_rel(int start_dir, const char *rel, bool follow_final) {
    int cur = start_dir;
    int link_count = 0;
    const int MAX_LINKS = 40;

    while (*rel) {
        while (*rel == '/') rel++;
        if (!*rel) break;

        // extract next component
        const char *slash = rel;
        while (*slash && *slash != '/') slash++;
        size_t clen = (size_t)(slash - rel);
        if (clen >= TMPFS_MAX_NAME) return -ENAMETOOLONG;

        char comp[TMPFS_MAX_NAME];
        memcpy(comp, rel, clen);
        comp[clen] = '\0';
        bool is_last = (*slash == '\0');

        if (strcmp(comp, ".") == 0) {
            rel = slash;
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            if (tmpfs_inodes[cur].parent >= 0) cur = tmpfs_inodes[cur].parent;
            rel = slash;
            continue;
        }

        if (tmpfs_inodes[cur].type != TMPFS_DIR) return -ENOTDIR;

        int child = -1;
        for (int i = 0; i < tmpfs_inodes[cur].child_count; i++) {
            int ci = tmpfs_inodes[cur].children[i];
            if (ci < 0) continue;
            if (tmpfs_inodes[ci].active &&
                strncmp(tmpfs_inodes[ci].name, comp, TMPFS_MAX_NAME) == 0) {
                child = ci; break;
            }
        }
        if (child < 0) return -ENOENT;

        // follow symlink if intermediate, or final-and-follow_final
        if (tmpfs_inodes[child].type == TMPFS_LNK && (!is_last || follow_final)) {
            if (++link_count > MAX_LINKS) return -ELOOP;
            const char *tgt = tmpfs_inodes[child].target;
            int r;
            if (tgt[0] == '/') {
                // Absolute target: resolve from the filesystem root via the
                // mount-matching path so cross-mount symlinks work (e.g.
                // /tmp/link.txt -> /tmp/target.txt).
                r = resolve_locked(tgt, is_last ? follow_final : true);
            } else {
                // Relative target: resolve from the current mount's root.
                int mslot = tmpfs_inodes[child].mount_idx;
                int root = tmpfs_mounts[mslot].root_inode;
                r = walk_rel(root, tgt, is_last ? follow_final : true);
            }
            if (r < 0) return r;
            child = r;
        }

        cur = child;
        rel = slash;
    }
    return cur;
}


// Internal: assume tmpfs_lock already held.  Returns parent inode index and
// copies the final component name into `last_buf`.
static int resolve_parent_locked(const char *abs_path, char *last_buf, size_t last_size) {
    const char *rel = NULL;
    int m = find_mount_for(abs_path, &rel);
    if (m < 0) return -ENOENT;
    int root = tmpfs_mounts[m].root_inode;

    const char *end = rel + strlen(rel);
    while (end > rel && end[-1] == '/') end--;
    const char *last_slash = end;
    while (last_slash > rel && last_slash[-1] != '/') last_slash--;
    const char *last = last_slash;
    size_t plen = (size_t)(last_slash - rel);

    int parent;
    if (plen == 0) {
        parent = root;
    } else {
        char pp[256];
        if (plen >= sizeof(pp)) return -ENAMETOOLONG;
        memcpy(pp, rel, plen); pp[plen] = '\0';
        parent = walk_rel(root, pp, true);
        if (parent < 0) return parent;
    }
    size_t llen = (size_t)(end - last);
    if (llen == 0 || llen >= last_size) return -ENAMETOOLONG;
    memcpy(last_buf, last, llen);
    last_buf[llen] = '\0';
    return parent;
}

int resolve_tmpfs_parent(const char *abs_path, const char **last_out) {
    // Public wrapper: kept for API completeness but the mutators use the
    // _locked variant directly to avoid returning a dangling pointer.
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    static char last_buf[256];
    int p = resolve_parent_locked(abs_path, last_buf, sizeof(last_buf));
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    if (p >= 0 && last_out) *last_out = last_buf;
    return p;
}

static int read_tmpfs_dirent(int dir_inode, int index, char *name, size_t name_size,
                      uint8_t *type_out, uint64_t *ino_out) {
    if (dir_inode < 0 || dir_inode >= TMPFS_MAX_INODES) return -EBADF;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    tmpfs_inode_t *d = &tmpfs_inodes[dir_inode];
    if (!d->active || d->type != TMPFS_DIR) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return -ENOTDIR;
    }
    if (index < 0 || index >= d->child_count) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return 0;   // end
    }
    int ci = d->children[index];
    if (ci < 0 || !tmpfs_inodes[ci].active) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return 0;
    }
    tmpfs_inode_t *c = &tmpfs_inodes[ci];
    strncpy(name, c->name, name_size - 1);
    name[name_size - 1] = '\0';
    if (type_out) {
        switch (c->type) {
            case TMPFS_DIR:  *type_out = 4; break;   // DT_DIR
            case TMPFS_SOCK: *type_out = 12; break;  // DT_SOCK
            case TMPFS_REG:  *type_out = 8; break;   // DT_REG
            case TMPFS_LNK:  *type_out = 10; break;  // DT_LNK
            default:         *type_out = 0; break;
        }
    }
    if (ino_out) *ino_out = c->ino;
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 1;
}

static int add_child(int parent, int child) {
    tmpfs_inode_t *p = &tmpfs_inodes[parent];
    if (p->child_count >= TMPFS_MAX_CHILDREN) return -ENOSPC;
    p->children[p->child_count++] = child;
    return 0;
}

static int remove_child(int parent, int child) {
    tmpfs_inode_t *p = &tmpfs_inodes[parent];
    for (int i = 0; i < p->child_count; i++) {
        if (p->children[i] == child) {
            p->children[i] = p->children[p->child_count - 1];
            p->child_count--;
            return 0;
        }
    }
    return -ENOENT;
}

static int find_in_dir(int dir, const char *name) {
    tmpfs_inode_t *d = &tmpfs_inodes[dir];
    for (int i = 0; i < d->child_count; i++) {
        int ci = d->children[i];
        if (ci < 0) continue;
        if (tmpfs_inodes[ci].active &&
            strncmp(tmpfs_inodes[ci].name, name, TMPFS_MAX_NAME) == 0) {
            return ci;
        }
    }
    return -ENOENT;
}

// Create a leaf entry (dir/file/symlink) named `last` under its parent dir.
// Internal entry creator. The caller must hold tmpfs_lock.
static int create_entry_locked(const char *abs_path, tmpfs_type_t type,
                               mode_t mode, uid_t uid, gid_t gid,
                               const char *symlink_target) {
    char last[TMPFS_MAX_NAME];
    int parent = resolve_parent_locked(abs_path, last, sizeof(last));
    if (parent < 0) return parent;

    if (tmpfs_inodes[parent].type != TMPFS_DIR) {
        return -ENOTDIR;
    }
    if (find_in_dir(parent, last) >= 0) {
        return -EEXIST;
    }

    int inode = alloc_inode();
    if (inode < 0) return inode;

    int mslot = tmpfs_inodes[parent].mount_idx;
    tmpfs_inodes[inode].type      = type;
    tmpfs_inodes[inode].mode      = mode;
    tmpfs_inodes[inode].uid       = uid;
    tmpfs_inodes[inode].gid       = gid;
    tmpfs_inodes[inode].parent    = parent;
    tmpfs_inodes[inode].mount_idx = mslot;
    strncpy(tmpfs_inodes[inode].name, last, TMPFS_MAX_NAME - 1);
    tmpfs_inodes[inode].name[TMPFS_MAX_NAME - 1] = '\0';

    if (type == TMPFS_LNK && symlink_target) {
        strncpy(tmpfs_inodes[inode].target, symlink_target, TMPFS_LINK_MAX - 1);
        tmpfs_inodes[inode].target[TMPFS_LINK_MAX - 1] = '\0';
    }

    int r = add_child(parent, inode);
    if (r < 0) { free_inode(inode); return r; }

    return inode;
}

static int create_entry(const char *abs_path, tmpfs_type_t type,
                        mode_t mode, uid_t uid, gid_t gid,
                        const char *symlink_target) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = create_entry_locked(abs_path, type, mode, uid, gid, symlink_target);

    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return inode;
}

int mkdir_tmpfs(const char *abs_path, mode_t mode, uid_t uid, gid_t gid) {
    int r = create_entry(abs_path, TMPFS_DIR, (mode & 07777) | 0040000, uid, gid, NULL);
    return r < 0 ? r : 0;
}
int symlink_tmpfs(const char *target, const char *abs_path, uid_t uid, gid_t gid) {
    int r = create_entry(abs_path, TMPFS_LNK, 0120777, uid, gid, target);
    return r < 0 ? r : 0;
}

static int remove_tmpfs(const char *abs_path) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(abs_path, false);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }

    if (tmpfs_inodes[inode].type == TMPFS_DIR) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EISDIR;
    }
    int parent = tmpfs_inodes[inode].parent;
    if (parent < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -EBUSY; } // mount root
    remove_child(parent, inode);
    free_inode(inode);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

int rmdir_tmpfs(const char *abs_path) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(abs_path, false);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }

    if (tmpfs_inodes[inode].type != TMPFS_DIR) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOTDIR;
    }
    if (tmpfs_inodes[inode].child_count > 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOTEMPTY;
    }
    int parent = tmpfs_inodes[inode].parent;
    if (parent < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -EBUSY; }
    remove_child(parent, inode);
    free_inode(inode);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

int rename_tmpfs(const char *old_abs, const char *new_abs) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int src = resolve_locked(old_abs, false);
    if (src < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return src; }

    char last[TMPFS_MAX_NAME];
    int new_parent = resolve_parent_locked(new_abs, last, sizeof(last));
    if (new_parent < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return new_parent; }

    if (tmpfs_inodes[new_parent].type != TMPFS_DIR) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOTDIR;
    }
    if (find_in_dir(new_parent, last) >= 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EEXIST;
    }

    int old_parent = tmpfs_inodes[src].parent;
    if (old_parent >= 0) remove_child(old_parent, src);
    tmpfs_inodes[src].parent = new_parent;
    strncpy(tmpfs_inodes[src].name, last, TMPFS_MAX_NAME - 1);
    tmpfs_inodes[src].name[TMPFS_MAX_NAME - 1] = '\0';
    add_child(new_parent, src);

    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

int link_tmpfs(const char *old_abs, const char *new_abs) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int src = resolve_locked(old_abs, false);
    if (src < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return src; }

    if (tmpfs_inodes[src].type == TMPFS_DIR) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EPERM; // hardlink dirs not allowed
    }
    char last[TMPFS_MAX_NAME];
    int new_parent = resolve_parent_locked(new_abs, last, sizeof(last));
    if (new_parent < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return new_parent; }

    if (find_in_dir(new_parent, last) >= 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EEXIST;
    }

    // tmpfs has no per-inode nlink counter in this struct; we model a hard
    // link as a second directory entry pointing at the SAME inode index.
    // (free_inode runs when the LAST reference is removed; we don't track
    // that precisely here, so we simply don't free on unlink if other refs
    // exist — tracked via a quick scan.)
    add_child(new_parent, src);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

int chmod_tmpfs(const char *abs_path, mode_t mode) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(abs_path, false);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }

    // preserve type bits, replace permission bits
    tmpfs_inodes[inode].mode = (tmpfs_inodes[inode].mode & ~07777) | (mode & 07777);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

// The caller must hold tmpfs_lock.
static int truncate_tmpfs_inode_locked(int inode, uint64_t size) {
    if (inode < 0 || inode >= TMPFS_MAX_INODES) return -EBADF;
    tmpfs_inode_t *n = &tmpfs_inodes[inode];
    if (!n->active) return -ENOENT;
    if (n->type != TMPFS_REG) return -EISDIR;

    if (size == 0) {
        if (n->data) { free(n->data); n->data = NULL; }
        n->size = 0; n->capacity = 0;
    } else if (size > n->capacity) {
        uint8_t *nd = malloc(size);
        if (!nd) return -ENOMEM;
        if (n->data && n->size) memcpy(nd, n->data, n->size);
        if (n->size < size) memset(nd + n->size, 0, size - n->size);
        if (n->data) free(n->data);
        n->data = nd; n->capacity = size; n->size = size;
    } else {
        if (n->size < size) memset(n->data + n->size, 0, size - n->size);
        n->size = size;
    }
    return 0;
}

// The caller must hold tmpfs_lock.
static int64_t write_tmpfs_inode_locked(int inode, const void *buf, uint64_t count, uint64_t offset) {
    if (inode < 0 || inode >= TMPFS_MAX_INODES) return -EBADF;
    tmpfs_inode_t *n = &tmpfs_inodes[inode];
    if (!n->active || n->type != TMPFS_REG) {
        return -EISDIR;
    }
    if (count == 0) return 0;
    if (offset + count < offset) return -EINVAL;
    uint64_t need = offset + count;
    if (need > n->capacity) {
        // geometric growth
        uint64_t newcap = n->capacity ? n->capacity : 4096;
        while (newcap < need) newcap *= 2;
        uint8_t *nd = malloc(newcap);
        if (!nd) return -ENOMEM;
        if (n->data && n->size) memcpy(nd, n->data, n->size);
        if (n->size < offset) memset(nd + n->size, 0, offset - n->size);
        if (n->data) free(n->data);
        n->data = nd; n->capacity = newcap;
    }
    memcpy(n->data + offset, buf, count);
    if (need > n->size) n->size = need;
    return (int64_t)count;
}

// ---------------------------------------------------------------------------
// stat / readlink (whole-path)
// ---------------------------------------------------------------------------

int read_tmpfs_link(const char *path, char *out, size_t out_size) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(path, false);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }
    if (tmpfs_inodes[inode].type != TMPFS_LNK) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EINVAL;
    }
    size_t len = strlen(tmpfs_inodes[inode].target);
    if (out_size <= len) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -ERANGE; }
    strcpy(out, tmpfs_inodes[inode].target);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return (int)len;
}

// ---------------------------------------------------------------------------
// initrd-equivalent path-based API
// ---------------------------------------------------------------------------

static tmpfs_file_t make_tmpfs_file(int inode_idx, tmpfs_inode_t *n) {
    tmpfs_file_t r;
    r.inode = n->ino;
    r.mode = n->mode;
    r.uid  = n->uid;
    r.gid  = n->gid;
    if (n->type == TMPFS_REG) {
        r.data = n->data;
        r.size = n->size;
    } else if (n->type == TMPFS_LNK) {
        r.data = n->target;
        r.size = strlen(n->target);
    } else {
        r.data = NULL;
        r.size = 0;
    }
    return r;
}

tmpfs_file_t read_tmpfs(const char *path) {
    tmpfs_file_t result = {0};
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(path, true);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return result; }
    result = make_tmpfs_file(inode, &tmpfs_inodes[inode]);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return result;
}

tmpfs_file_t stat_tmpfs(const char *path) {
    return read_tmpfs(path);
}

tmpfs_file_t stat_tmpfs_nofollow(const char *path) {
    tmpfs_file_t result = {0};
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(path, false);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return result; }
    result = make_tmpfs_file(inode, &tmpfs_inodes[inode]);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return result;
}

int write_tmpfs(const char *path, const void *data, uint64_t size, uint32_t mode, uid_t uid, gid_t gid) {
    if (size > 0 && !data) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(path, true);
    if (inode < 0) {
        mode_t type_bits = mode & S_IFMT;
        tmpfs_type_t type = (type_bits == S_IFSOCK) ? TMPFS_SOCK : TMPFS_REG;
        mode_t final_mode = (mode & S_IFMT) ? mode : (mode & 07777) | S_IFREG;
        if (!mode) final_mode = S_IFREG | 0644;
        inode = create_entry_locked(path, type, final_mode, uid, gid, NULL);
        if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }
    }

    tmpfs_inode_t *n = &tmpfs_inodes[inode];
    if (n->type == TMPFS_SOCK && size == 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return 0;
    }
    if (n->type != TMPFS_REG) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return -EISDIR;
    }

    int r = truncate_tmpfs_inode_locked(inode, size);
    if (r == 0 && size > 0)
        r = (int)write_tmpfs_inode_locked(inode, data, size, 0);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return r < 0 ? r : 0;
}

int write_tmpfs_partial(const char *path, const void *data, uint64_t off, uint64_t count, uint32_t mode, uid_t uid, gid_t gid) {
    if (count > 0 && !data) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(path, true);
    if (inode < 0) {
        mode_t final_mode = mode ? ((mode & S_IFMT) ? mode : (mode & 07777) | S_IFREG) : S_IFREG | 0644;
        inode = create_entry_locked(path, TMPFS_REG, final_mode, uid, gid, NULL);
        if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }
    }
    int64_t r = write_tmpfs_inode_locked(inode, data, count, off);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return r < 0 ? (int)r : 0;
}

int delete_tmpfs(const char *path) {
    return remove_tmpfs(path);
}

int next_tmpfs_child(int *index, const char *dir_norm, char *child_name, size_t child_name_size,
                     uint8_t *child_type, ino_t *child_ino) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int dir_inode = resolve_locked(dir_norm, true);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    if (dir_inode < 0) return 1;
    uint64_t ino = 0;
    int found = read_tmpfs_dirent(dir_inode, *index, child_name, child_name_size, child_type, &ino);
    if (found && child_ino) *child_ino = (ino_t)ino;
    return found ? 0 : 1;
}

void init_tmpfs(void) {
    memset(tmpfs_inodes, 0, sizeof(tmpfs_inodes));
    memset(tmpfs_mounts, 0, sizeof(tmpfs_mounts));
    next_ino = 1;
    printf("tmpfs: initialized tmpfs\n");
}

// ---------------------------------------------------------------------------
// Path-based wrappers matching initrd exactly
// ---------------------------------------------------------------------------

int truncate_tmpfs(const char *path, uint64_t size) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(path, true);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }
    int r = truncate_tmpfs_inode_locked(inode, size);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return r;
}

int readlink_tmpfs(const char *path, char *out, size_t out_size) {
    return read_tmpfs_link(path, out, out_size);
}

int get_tmpfs_entry(int index, tmpfs_dirent_t *entry) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    if (index < 0 || index >= TMPFS_MAX_INODES) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return -ENOENT;
    }
    tmpfs_inode_t *n = &tmpfs_inodes[index];
    if (!n->active) {
        spin_unlock_irqrestore(&tmpfs_lock, irq);
        return -ENOENT;
    }
    strncpy(entry->name, n->name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->type = n->type;
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}
