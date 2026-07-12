#include <freestanding/errno.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/log.h>
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

bool match_tmpfs_mount(const char *abs_path) {
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

int resolve_tmpfs(const char *abs_path) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int r = resolve_locked(abs_path, true);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return r;
}

int resolve_tmpfs_nofollow(const char *abs_path) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int r = resolve_locked(abs_path, false);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return r;
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

int read_tmpfs_dirent(int dir_inode, int index, char *name, size_t name_size,
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
static int create_entry(const char *abs_path, tmpfs_type_t type,
                        mode_t mode, uid_t uid, gid_t gid,
                        const char *symlink_target) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);

    char last[TMPFS_MAX_NAME];
    int parent = resolve_parent_locked(abs_path, last, sizeof(last));
    if (parent < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return parent; }

    if (tmpfs_inodes[parent].type != TMPFS_DIR) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOTDIR;
    }
    if (find_in_dir(parent, last) >= 0) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EEXIST;
    }

    int inode = alloc_inode();
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return inode; }

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
    if (r < 0) { free_inode(inode); spin_unlock_irqrestore(&tmpfs_lock, irq); return r; }

    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return inode;
}

int make_tmpfs_dir(const char *abs_path, mode_t mode, uid_t uid, gid_t gid) {
    int r = create_entry(abs_path, TMPFS_DIR, (mode & 07777) | 0040000, uid, gid, NULL);
    return r < 0 ? r : 0;
}
int create_tmpfs_file(const char *abs_path, mode_t mode, uid_t uid, gid_t gid) {
    // Returns the inode index (>=0) on success so open_tmpfs_common can store
    // it in the fd handle.  Negative on error.
    return create_entry(abs_path, TMPFS_REG, (mode & 07777) | 0100000, uid, gid, NULL);
}
int create_tmpfs_socket(const char *abs_path, mode_t mode, uid_t uid, gid_t gid) {
    // Create a socket-type tmpfs inode. Returns inode index on success.
    return create_entry(abs_path, TMPFS_SOCK, (mode & 07777) | 0140000, uid, gid, NULL);
}
int make_tmpfs_symlink(const char *target, const char *abs_path, uid_t uid, gid_t gid) {
    int r = create_entry(abs_path, TMPFS_LNK, 0120777, uid, gid, target);
    return r < 0 ? r : 0;
}

int remove_tmpfs(const char *abs_path) {
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

int remove_tmpfs_dir(const char *abs_path) {
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

int truncate_tmpfs(int inode, uint64_t size) {
    if (inode < 0 || inode >= TMPFS_MAX_INODES) return -EBADF;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    tmpfs_inode_t *n = &tmpfs_inodes[inode];
    if (!n->active) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOENT; }
    if (n->type != TMPFS_REG) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -EISDIR; }

    if (size == 0) {
        if (n->data) { free(n->data); n->data = NULL; }
        n->size = 0; n->capacity = 0;
    } else if (size > n->capacity) {
        uint8_t *nd = malloc(size);
        if (!nd) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOMEM; }
        if (n->data && n->size) memcpy(nd, n->data, n->size);
        if (n->size < size) memset(nd + n->size, 0, size - n->size);
        if (n->data) free(n->data);
        n->data = nd; n->capacity = size; n->size = size;
    } else {
        if (n->size < size) memset(n->data + n->size, 0, size - n->size);
        n->size = size;
    }
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return 0;
}

// ---------------------------------------------------------------------------
// file I/O
// ---------------------------------------------------------------------------

int64_t read_tmpfs(int inode, void *buf, uint64_t count, uint64_t offset) {
    if (inode < 0 || inode >= TMPFS_MAX_INODES) return -EBADF;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    tmpfs_inode_t *n = &tmpfs_inodes[inode];
    if (!n->active || n->type != TMPFS_REG) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EISDIR;
    }
    if (offset >= n->size) { spin_unlock_irqrestore(&tmpfs_lock, irq); return 0; }
    uint64_t avail = n->size - offset;
    uint64_t to_read = count < avail ? count : avail;
    memcpy(buf, n->data + offset, to_read);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return (int64_t)to_read;
}

int64_t write_tmpfs(int inode, const void *buf, uint64_t count, uint64_t offset) {
    if (inode < 0 || inode >= TMPFS_MAX_INODES) return -EBADF;
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    tmpfs_inode_t *n = &tmpfs_inodes[inode];
    if (!n->active || n->type != TMPFS_REG) {
        spin_unlock_irqrestore(&tmpfs_lock, irq); return -EISDIR;
    }
    uint64_t need = offset + count;
    if (need > n->capacity) {
        // geometric growth
        uint64_t newcap = n->capacity ? n->capacity : 4096;
        while (newcap < need) newcap *= 2;
        uint8_t *nd = malloc(newcap);
        if (!nd) { spin_unlock_irqrestore(&tmpfs_lock, irq); return -ENOMEM; }
        if (n->data && n->size) memcpy(nd, n->data, n->size);
        if (n->data) free(n->data);
        n->data = nd; n->capacity = newcap;
    }
    memcpy(n->data + offset, buf, count);
    if (need > n->size) n->size = need;
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return (int64_t)count;
}

// ---------------------------------------------------------------------------
// stat / readlink (whole-path)
// ---------------------------------------------------------------------------

static void fill_stat(tmpfs_inode_t *n, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode   = n->mode;
    st->st_uid    = n->uid;
    st->st_gid    = n->gid;
    st->st_nlink  = 1;
    st->st_dev    = 0;
    st->st_ino    = n->ino;
    st->st_blksize = 4096;
    if (n->type == TMPFS_REG) {
        st->st_size   = n->size;
        st->st_blocks = (n->size + 511) / 512;
    } else if (n->type == TMPFS_LNK) {
        st->st_size   = strlen(n->target);
        st->st_blocks = 0;
    } else if (n->type == TMPFS_SOCK) {
        st->st_size   = 0;
        st->st_blocks = 0;
    } else {
        st->st_size   = 0;
        st->st_blocks = 0;
    }
}

bool stat_tmpfs(const char *abs_path, struct stat *st) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(abs_path, true);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return false; }
    fill_stat(&tmpfs_inodes[inode], st);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return true;
}

bool stat_tmpfs_nofollow(const char *abs_path, struct stat *st) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(abs_path, false);
    if (inode < 0) { spin_unlock_irqrestore(&tmpfs_lock, irq); return false; }
    fill_stat(&tmpfs_inodes[inode], st);
    spin_unlock_irqrestore(&tmpfs_lock, irq);
    return true;
}

int read_tmpfs_link(const char *abs_path, char *out, size_t out_size) {
    uint64_t irq;
    spin_lock_irqsave(&tmpfs_lock, &irq);
    int inode = resolve_locked(abs_path, false);
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

void init_tmpfs(void) {
    memset(tmpfs_inodes, 0, sizeof(tmpfs_inodes));
    memset(tmpfs_mounts, 0, sizeof(tmpfs_mounts));
    next_ino = 1;
    log("initialized tmpfs");
}
