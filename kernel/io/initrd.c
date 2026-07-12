#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <freestanding/sys/stat.h>
#include <freestanding/sys/types.h>
#include <main/panic.h>
#include <io/initrd.h>
#include <main/string.h>
#include <main/gzip.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <freestanding/errno.h>
#include <freestanding/dirent.h>
#include <main/sched.h>
#include <main/limine_req.h>
#include <main/log.h>
#include <limine/limine.h>

struct initrd_archive_entry {
    char path[256];
    uint8_t *data;
    uint64_t size;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    uint32_t ino;
    uint32_t devmajor;
    uint32_t devminor;
    uint32_t nlink;
    char link_target[256];
};

static uint8_t *initrd_decompressed = NULL;
static struct initrd_archive_entry *archive_entries = NULL;
static int archive_entry_count = 0;
// One byte per sorted archive entry. Deleted archive-backed paths are hidden
// without consuming one of the fixed-size overlay slots.
static uint8_t *archive_tombstone_bits = NULL;
static modified_file_t modified_files[MAX_MODIFIED_FILES];

static void collapse_slashes(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    bool last_was_slash = false;

    for (size_t i = 0; in[i] && j + 1 < out_size; i++) {
        if (in[i] == '/') { if (last_was_slash) continue; last_was_slash = true; } else {
            last_was_slash = false;
        }

        out[j++] = in[i];
    }

    if (j == 0) {
        out[j++] = '/';
    } else if (j > 1 && out[j - 1] == '/') {
        j--;
    }

    out[j] = '\0';
}

static void normalize_abs_components(const char *in_abs, char *out_abs, size_t out_size) {
    const char *parts[64];
    char tmp[256];
    int depth = 0;

    strncpy(tmp, in_abs, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';

        if (strcmp(start, ".") == 0) {
            continue;
        }
        if (strcmp(start, "..") == 0) { if (depth > 0) depth--; continue; }
        if (depth < 64) parts[depth++] = start;
    }

    if (depth == 0) {
        strncpy(out_abs, "/", out_size - 1);
        out_abs[out_size - 1] = '\0';
        return;
    }

    out_abs[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strncat(out_abs, "/", out_size - strlen(out_abs) - 1);
        strncat(out_abs, parts[i], out_size - strlen(out_abs) - 1);
    }
}

static void normalize_path(const char *in_abs, char *out, size_t out_size) {
    if (strcmp(in_abs, "/") == 0) {
        strcpy(out, "./");
    } else {
        out[0] = '.';
        strncpy(out + 1, in_abs, out_size - 2);
    }
    out[out_size - 1] = '\0';
}

void resolve_link_target(const char *base_path_abs, const char *link_target, char *out_abs, size_t out_size) {
    if (link_target[0] == '/') {
        strncpy(out_abs, link_target, out_size - 1);
        out_abs[out_size - 1] = '\0';
        return;
    }
    strncpy(out_abs, base_path_abs, out_size - 1);
    out_abs[out_size - 1] = '\0';

    char *last_slash = strrchr(out_abs, '/');
    if (last_slash) { last_slash[1] = '\0'; strncat(out_abs, link_target, out_size - strlen(out_abs) - 1); } else {
        strncpy(out_abs, link_target, out_size - 1);
        out_abs[out_size - 1] = '\0';
    }
}

static int compare_archive_names(const char *a, const char *b) {
    while (*a == '.' && a[1] == '/') a += 2;
    while (*b == '.' && b[1] == '/') b += 2;
    while (*a == '/') a++;
    while (*b == '/') b++;
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void swap_entries(struct initrd_archive_entry *a, struct initrd_archive_entry *b) {
    struct initrd_archive_entry t = *a; *a = *b; *b = t;
}

static void quicksort_archive_entries(struct initrd_archive_entry *arr, int low, int high) {
    if (low < high) {
        int i = low - 1;
        for (int j = low; j < high; j++) {
            if (compare_archive_names(arr[j].path, arr[high].path) < 0) {
                i++;
                swap_entries(&arr[i], &arr[j]);
            }
        }
        swap_entries(&arr[i + 1], &arr[high]);
        int pi = i + 1;
        quicksort_archive_entries(arr, low, pi - 1);
        quicksort_archive_entries(arr, pi + 1, high);
    }
}

static int archive_entry_idx(const char *norm_path) {
    if (!archive_entries || archive_entry_count <= 0) return -1;
    int low = 0, high = archive_entry_count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = compare_archive_names(archive_entries[mid].path, norm_path);
        if (cmp == 0) return mid;
        if (cmp < 0) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

static bool archive_tombstone_get(const char *norm_path) {
    int idx = archive_entry_idx(norm_path);
    return idx >= 0 && archive_tombstone_bits && archive_tombstone_bits[idx] != 0;
}

static void archive_tombstone_set(const char *norm_path) {
    int idx = archive_entry_idx(norm_path);
    if (idx >= 0 && archive_tombstone_bits) archive_tombstone_bits[idx] = 1;
}

static void archive_tombstone_clear(const char *norm_path) {
    int idx = archive_entry_idx(norm_path);
    if (idx >= 0 && archive_tombstone_bits) archive_tombstone_bits[idx] = 0;
}

static bool find_archive_symlink(const char *abs_path, char *resolved_abs, size_t resolved_size) {
    char norm[256];
    normalize_path(abs_path, norm, sizeof(norm));

    // Check overlay symlinks first (created via symlink_initrd)
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && !modified_files[i].is_tombstone &&
            (modified_files[i].mode & 0xF000) == 0xA000 &&
            compare_archive_names(modified_files[i].path, norm) == 0) {
            char target[101];
            strncpy(target, (const char *)modified_files[i].data, 100);
            target[100] = '\0';
            resolve_link_target(abs_path, target, resolved_abs, resolved_size);
            return true;
        }
    }

    int idx = archive_entry_idx(norm);
    if (idx < 0 || archive_tombstone_get(norm)) return false;
    struct initrd_archive_entry *entry = &archive_entries[idx];
    if ((entry->mode & 0xF000) != 0xA000) return false;
    strncpy(resolved_abs, entry->link_target, resolved_size - 1);
    resolved_abs[resolved_size - 1] = '\0';
    return true;
}

static void resolve_path_symlinks_ex(const char *in_abs, char *out_abs, size_t out_size, bool follow_final);

// Resolves symlinks in in_abs. When follow_final is false, the last path
// component is left untouched (lstat()/readlink()/unlink-of-a-symlink
// semantics); directory symlinks in the prefix are still followed.
static void resolve_path_symlinks_ex(const char *in_abs, char *out_abs, size_t out_size, bool follow_final) {
    char current[256];
    strncpy(current, in_abs, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    size_t total_len = strlen(current);
    // Drop a trailing slash so the real final component is the one we may skip.
    if (total_len > 1 && current[total_len - 1] == '/') {
        current[--total_len] = '\0';
    }

    for (int depth = 0; depth < 8; depth++) {
        bool changed = false;
        size_t len = strlen(current);

        for (size_t i = 1; i <= len; i++) {
            if (current[i] != '/' && current[i] != '\0') continue;

            // When not following the final component, skip the boundary that
            // marks the end of the whole path.
            if (!follow_final && current[i] == '\0') break;

            char prefix[256];
            if (i >= sizeof(prefix)) break;
            strncpy(prefix, current, i);
            prefix[i] = '\0';

            char target[256];
            if (find_archive_symlink(prefix, target, sizeof(target))) {
                char resolved_target[256];
                resolve_link_target(prefix, target, resolved_target, sizeof(resolved_target));

                char clean_target[256];
                collapse_slashes(resolved_target, clean_target, sizeof(clean_target));

                char next[256];
                normalize_abs_components(clean_target, next, sizeof(next));

                if (current[i] != '\0') {
                    if (strcmp(next, "/") == 0) {
                        strncat(next, current + i + 1, sizeof(next) - strlen(next) - 1);
                    } else {
                        strncat(next, current + i, sizeof(next) - strlen(next) - 1);
                    }
                }

                strncpy(current, next, sizeof(current) - 1);
                current[sizeof(current) - 1] = '\0';
                changed = true;
                break;
            }
        }

        if (!changed) break;
    }

    strncpy(out_abs, current, out_size - 1);
    out_abs[out_size - 1] = '\0';
}

static void add_modified_file(const char *path, void *data, size_t size, size_t capacity, uint32_t mode, uid_t uid, gid_t gid) {
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, path) == 0) {
            if (modified_files[i].data) free(modified_files[i].data);
            modified_files[i].data = data;
            modified_files[i].size = size;
            modified_files[i].capacity = capacity;
            modified_files[i].mode = mode;
            modified_files[i].uid = uid;
            modified_files[i].gid = gid;
            modified_files[i].is_tombstone = false;
            // Recreating a previously-deleted tar-backed path must clear the
            // tombstone, otherwise the new overlay entry would never be seen
            // (callers consult the tombstone bitmap directly via
            archive_tombstone_clear(path);
            return;
        }
    }

    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) {
            modified_files[i].is_active = true;
            strncpy(modified_files[i].path, path, sizeof(modified_files[i].path) - 1);
            modified_files[i].path[sizeof(modified_files[i].path) - 1] = '\0';
            modified_files[i].data = data;
            modified_files[i].size = size;
            modified_files[i].capacity = capacity;
            modified_files[i].mode = mode;
            modified_files[i].uid = uid;
            modified_files[i].gid = gid;
            modified_files[i].is_tombstone = false;
            archive_tombstone_clear(path);
            return;
        }
    }
}

// Drop a tombstone: mark the sorted archive entry for `norm_path` (already in
// "./foo/bar" form) as deleted so read/stat/getdents hide the original.
// Uses the per-entry bitmap instead of the overlay array, so there is no
// MAX_MODIFIED_FILES ceiling and "rm -rf /" no longer exhausts the table.
static void add_tombstone(const char *norm_path) {
    archive_tombstone_set(norm_path);
}

// Returns true if the sorted tar index table contains `norm_path`.
// Uses the binary-search index (O(log n)) instead of the old linear walk.
static bool archive_has_entry(const char *norm_path) {
    return archive_entry_idx(norm_path) >= 0;
}

static void get_norm_path_ex(const char *path, char *out_norm, size_t out_size, bool follow_final);

static void get_norm_path(const char *path, char *out_norm, size_t out_size) {
    get_norm_path_ex(path, out_norm, out_size, true);
}

static void get_norm_path_ex(const char *path, char *out_norm, size_t out_size, bool follow_final) {
    size_t len = strlen(path);
    if (!follow_final) {
        if (len > 0 && path[len - 1] == '/') {
            follow_final = true;
        } else if (len >= 2 && path[len - 1] == '.' && path[len - 2] == '/') {
            follow_final = true;
        } else if (len >= 3 && path[len - 1] == '.' && path[len - 2] == '.' && path[len - 3] == '/') {
            follow_final = true;
        } else if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0) {
            follow_final = true;
        }
    }

    char abs_path[256];
    get_absolute_path(path, abs_path, sizeof(abs_path));

    char clean_path[256];
    collapse_slashes(abs_path, clean_path, sizeof(clean_path));

    char normalized_path[256];
    normalize_abs_components(clean_path, normalized_path, sizeof(normalized_path));

    char resolved_path[256];
    resolve_path_symlinks_ex(normalized_path, resolved_path, sizeof(resolved_path), follow_final);

    normalize_path(resolved_path, out_norm, out_size);
}

void get_absolute_path(const char *in, char *out_abs, size_t out_size) {
    if (in[0] == '/') { strncpy(out_abs, in, out_size - 1); } else {
        if (current_task_ptr) {
            strncpy(out_abs, current_task_ptr->cwd, out_size - 1);
            if (strcmp(out_abs, "/") != 0) strncat(out_abs, "/", out_size - strlen(out_abs) - 1);
        } else { strcpy(out_abs, "/"); }
        strncat(out_abs, in, out_size - strlen(out_abs) - 1);
    }
    out_abs[out_size - 1] = '\0';
}

initrd_file_t read_initrd(const char *path) {
    char current_path[256];
    strncpy(current_path, path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    initrd_file_t result = { .data = NULL, .size = 0, .mode = 0, .uid = 0, .gid = 0 };

    for (int depth = 0; depth < 8; depth++) {
        char norm[256];
        get_norm_path(current_path, norm, sizeof(norm));

        // 1. Check overlay (live entries only; tombstones live in the bitmap
        //    so modified_files[] never carries an is_tombstone marker anymore).
        bool found_link = false;
        for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
            if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
                if ((modified_files[i].mode & 0xF000) == 0xA000) {
                    char current_abs[256];
                    get_absolute_path(current_path, current_abs, sizeof(current_abs));
                    resolve_link_target(current_abs, (char*)modified_files[i].data, current_path, sizeof(current_path));
                    found_link = true;
                    break;
                }
                result.data = modified_files[i].data;
                result.size = modified_files[i].size;
                result.mode = modified_files[i].mode;
                result.uid = modified_files[i].uid;
                result.gid = modified_files[i].gid;
                return result;
            }
        }
        if (found_link) continue;

        if (archive_tombstone_get(norm)) return result;

        int idx = archive_entry_idx(norm);
        if (idx >= 0) {
            struct initrd_archive_entry *entry = &archive_entries[idx];
            if ((entry->mode & 0xF000) == 0xA000) {
                char current_abs[256];
                get_absolute_path(current_path, current_abs, sizeof(current_abs));
                resolve_link_target(current_abs, entry->link_target, current_path, sizeof(current_path));
                found_link = true;
                continue;
            }
            result.data = entry->data;
            result.size = entry->size;
            result.mode = entry->mode;
            result.uid = entry->uid;
            result.gid = entry->gid;
            return result;
        }
        if (!found_link) break;
    }

    return result;
}

static initrd_file_t stat_initrd_ex(const char *path, bool follow_final) {
    char norm[256];
    // Normalizes the path and resolves directory symlinks in the prefix,
    // optionally resolving the final component too (follow_final).
    get_norm_path_ex(path, norm, sizeof(norm), follow_final);

    // check overlay first
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            initrd_file_t result = {
                .data = modified_files[i].data,
                .size = modified_files[i].size,
                .mode = modified_files[i].mode,
                .uid  = modified_files[i].uid,
                .gid  = modified_files[i].gid,
            };
            return result;
        }
    }

    // Tombstoned archive entry: deleted.
    if (archive_tombstone_get(norm)) {
        initrd_file_t empty = {0};
        return empty;
    }

    int idx = archive_entry_idx(norm);
    if (idx >= 0) {
        struct initrd_archive_entry *entry = &archive_entries[idx];
        initrd_file_t result = {
            .data = ((entry->mode & 0xF000) == 0xA000) ? (void *)entry->link_target : (void *)entry->data,
            .size = entry->size,
            .mode = entry->mode,
            .uid = entry->uid,
            .gid = entry->gid,
        };
        return result;
    }

    initrd_file_t empty = {0};
    return empty;
}

initrd_file_t stat_initrd(const char *path) {
    return stat_initrd_ex(path, true);
}

initrd_file_t stat_initrd_nofollow(const char *path) {
    size_t len = strlen(path);
    bool follow_final = false;
    if (len > 0 && path[len - 1] == '/') {
        follow_final = true;
    }
    return stat_initrd_ex(path, follow_final);
}

int write_initrd(const char *path, const void *data, uint64_t size, uint32_t mode, uid_t uid, gid_t gid) {
    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    void *copy = NULL;
    if (size > 0) {
        copy = malloc((size_t)size);
        if (!copy) return -ENOMEM;
        memcpy(copy, data, (size_t)size);
    }

    add_modified_file(norm, copy, (size_t)size, (size_t)size, mode, uid, gid);
    return 0;
}

// Find an active (non-tombstone) overlay entry for `norm`, or NULL.
static modified_file_t *find_overlay_entry(const char *norm) {
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active &&
            !modified_files[i].is_tombstone &&
            strcmp(modified_files[i].path, norm) == 0) {
            return &modified_files[i];
        }
    }
    return NULL;
}

// In-place partial write at byte offset `off`.  Critical for performance:
// write_initrd() above takes a full-replacement buffer and malloc+memcpy of
// the whole file each call, which makes dd-style loops (many small writes
// to a growing file) O(N^2) — a 1.5 MiB image via `dd bs=512` took ~35 s.
//
// This helper reuses the existing overlay buffer when one exists: it only
// reallocates (geometrically, 1.5x) when the write extends past the current
// allocation, and copies only the newly-written bytes.  Each write is thus
// O(off+count) amortized, not O(file_size).  When no overlay entry exists
// yet (first write on a tar-backed or brand-new file), it snapshots the
// current contents, applies the patch, and stores a fresh overlay entry.
int write_initrd_partial(const char *path, const void *data, uint64_t off, uint64_t count, uint32_t mode, uid_t uid, gid_t gid) {
    if (count == 0) return 0;

    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    if (off + count < count) return -EINVAL; // overflow guard

    modified_file_t *ent = find_overlay_entry(norm);
    if (ent) {
        uint64_t new_size = off + count;
        if (new_size > ent->capacity) {
            // Grow geometrically (1.5x) to absorb future small writes
            // without a realloc each time.  Never shrink; the goal is to
            // stabilise large-file copy loops like dd/tar.
            uint64_t grow = ent->capacity * 2;
            if (grow < new_size) grow = new_size + (new_size >> 1);
            if (grow < 512) grow = 512;
            uint64_t cap = grow;
            void *realloced = realloc(ent->data, (size_t)cap);
            if (!realloced) {
                // Fall back to exact-size allocation.
                realloced = realloc(ent->data, (size_t)new_size);
                if (!realloced) return -ENOMEM;
                cap = new_size;
            }
            ent->data = realloced;
            ent->capacity = cap;
        }
        if (new_size > ent->size) {
            // Zero-fill the gap if the write starts past the old end
            // (sparse write, e.g. lseek+write beyond EOF).
            if (off > ent->size) {
                memset((uint8_t *)ent->data + ent->size, 0, (size_t)(off - ent->size));
            }
            // Track true file size, not allocation capacity.
            ent->size = new_size;
        }
        memcpy((uint8_t *)ent->data + off, data, (size_t)count);
        ent->mode = mode ? mode : (ent->mode ? ent->mode : 0100644);
        ent->uid  = uid;
        ent->gid  = gid;
        return 0;
    }

    // No overlay entry yet.  Snapshot current contents (tar-backed or empty),
    // apply the patch into a fresh buffer, and store it as a new overlay entry.
    initrd_file_t cur = read_initrd(path);
    uint64_t base_size = cur.size;
    uint64_t need = off + count;
    uint64_t alloc = (base_size > need) ? base_size : need;

    void *buf = malloc((size_t)alloc);
    if (!buf) return -ENOMEM;

    // Seed with current file contents if any (this promotes tar-backed data
    // into the overlay so future writes hit the fast path above).
    if (cur.data && base_size) memcpy(buf, cur.data, (size_t)base_size);
    // Zero-fill any tail-gap before the write region.
    if (base_size < off) memset((uint8_t *)buf + base_size, 0, (size_t)(off - base_size));
    // Apply the patch.
    memcpy((uint8_t *)buf + off, data, (size_t)count);

    uint32_t final_mode = mode ? mode : (cur.mode ? cur.mode : 0100644);
    add_modified_file(norm, buf, (size_t)need, (size_t)alloc, final_mode, uid, gid);
    return 0;
}

int mkdir_initrd(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    add_modified_file(norm, NULL, 0, 0, mode | 0040000, uid, gid);
    return 0;
}


int delete_initrd(const char *path) {
    // Don't follow the final component: unlink must remove the entry itself
    // (including a symlink), not its target. Intermediate directory symlinks
    // are still resolved.
    char norm[256];
    get_norm_path_ex(path, norm, sizeof(norm), false);

    // If it lives in the overlay, just drop that entry. Re-creating the path
    // later will clear the slot via add_modified_file().
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            if (modified_files[i].data) free(modified_files[i].data);
            modified_files[i].is_active = false;
            // If an archive original also exists below it, leave a tombstone so it
            // doesn't resurface. Otherwise there's nothing to shadow.
            if (archive_has_entry(norm)) add_tombstone(norm);
            return 0;
        }
    }

    // Not in the overlay — but if the archive has it, record a tombstone so
    // the original is hidden from read/stat/getdents from now on.
    if (archive_has_entry(norm)) {
        add_tombstone(norm);
        return 0;
    }

    return -1; // Not found
}

int rmdir_initrd(const char *path) {
    // Don't follow the final component: rmdir on a symlink should fail with
    // ENOTDIR, not remove the target directory.
    char norm[256];
    get_norm_path_ex(path, norm, sizeof(norm), false);

    // check it exists and is a directory
    initrd_file_t file = stat_initrd(path);
    if (!file.mode) return -ENOENT;
    if ((file.mode & 0xF000) != 0x4000) return -ENOTDIR;

    // check it's empty — scan modified_files for any live (non-tombstone) children
    size_t norm_len = strlen(norm);
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) continue;
        if (modified_files[i].is_tombstone) continue;
        if (strcmp(modified_files[i].path, norm) == 0) continue; // itself
        if (strncmp(modified_files[i].path, norm, norm_len) == 0 &&
            modified_files[i].path[norm_len] == '/') return -ENOTEMPTY;
    }

    // Check archive entries for children. The sorted index lets us
    // binary-search for the first entry >= "<norm>/" and then walk forward
    // only as long as the prefix matches — O(log N + matches) instead of a
    // full O(N) linear scan per rmdir() call. This is what made `rm -rf`
    // slow: every directory removal re-walked the whole 6k-entry archive.
    if (archive_entries && archive_entry_count > 0) {
        char key[258];
        size_t klen = norm_len;
        if (klen + 1 < sizeof(key)) {
            memcpy(key, norm, klen);
            key[klen] = '/';
            key[klen + 1] = '\0';
        } else {
            memcpy(key, norm, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
        }

        int low = 0, high = archive_entry_count - 1, first = archive_entry_count;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            if (compare_archive_names(archive_entries[mid].path, key) < 0) low = mid + 1;
            else { first = mid; high = mid - 1; }
        }

        // Walk forward from `first` while the entry is a child of <norm>.
        // Because the array is sorted, once we pass the children range we
        // can stop immediately — no need to scan the rest of the archive.
        for (int i = first; i < archive_entry_count; i++) {
            struct initrd_archive_entry *entry = &archive_entries[i];
            // If this entry doesn't start with "<norm>/", we've passed the
            // children block (sorted order) — stop scanning.
            if (strncmp(entry->path, norm, norm_len) != 0 ||
                entry->path[norm_len] != '/') break;

            // Skip tombstoned (deleted) children — they're invisible.
            if (archive_tombstone_bits && archive_tombstone_bits[i]) continue;

            // Direct child: directory is not empty.
            if (strlen(entry->path) > norm_len + 1) return -ENOTEMPTY;
        }
    }

    // Remove it. If it's an overlay entry, drop the entry and leave a tombstone
    // if a tar original sits below it (so it doesn't resurface). If it's
    // tar-backed only, a tombstone is the only way to hide the original.
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            if (modified_files[i].data) free(modified_files[i].data);
            modified_files[i].is_active = false;
            if (archive_has_entry(norm)) add_tombstone(norm);
            return 0;
        }
    }

    if (archive_has_entry(norm)) {
        add_tombstone(norm);
        return 0;
    }

    return -ENOENT;
}

int symlink_initrd(const char *target, const char *path, uid_t uid, gid_t gid) {
    // Don't follow the final component: the symlink itself is being created.
    char norm[256];
    get_norm_path_ex(path, norm, sizeof(norm), false);

    size_t len = strlen(target);
    char *copy = malloc(len + 1);
    if (!copy) return -ENOMEM;
    strcpy(copy, target);

    add_modified_file(norm, copy, len, len, 0xA000 | 0777, uid, gid); // S_IFLNK
    return 0;
}

int chmod_initrd(const char *path, mode_t mode) {
    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    // Search for the entry in the overlay and update its mode in-place.
    // We must NOT call read_initrd + add_modified_file here because
    // read_initrd returns the same data pointer stored in the overlay,
    // and add_modified_file would free(old_data) then store the
    // now-dangling pointer back — use-after-free.
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            mode_t type_bits = (modified_files[i].mode & 0xF000);
            modified_files[i].mode = (mode & 0777) | type_bits;
            return 0;
        }
    }

    // Entry not in overlay — check the archive and promote it to
    // the overlay so the new mode persists.
    initrd_file_t file = read_initrd(path);
    if (!file.mode) { return -ENOENT; }

    mode_t type_bits = (file.mode & 0xF000);
    mode_t new_mode = (mode & 0777) | type_bits;

    // file.data from the archive is not owned by the overlay, so
    // copy it before handing it to add_modified_file.
    void *data_copy = NULL;
    if (file.data && file.size > 0) {
        data_copy = malloc(file.size);
        if (!data_copy) return -ENOMEM;
        memcpy(data_copy, file.data, file.size);
    }

    add_modified_file(norm, data_copy, file.size, file.size, new_mode, file.uid, file.gid);
    return 0;
}

int get_initrd_entry(int index, directory_entry_t *entry) {
    if (!entry) return -1;

    int count = 0;

    for (int i = 0; i < archive_entry_count; i++) {
        struct initrd_archive_entry *archive = &archive_entries[i];
        if (archive_tombstone_bits && archive_tombstone_bits[i]) continue;
        if (count == index) {
            strncpy(entry->name, archive->path, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            mode_t type = archive->mode & 0xF000;
            entry->type = (type == 0x4000) ? FT_DIRECTORY :
                          (type == 0xA000) ? FT_SYMLINK : FT_FILE;
            return 0;
        }
        count++;
    }

    // Phase 2: overlay (modified_files[]) entries. Tombstones are deletion
    // markers, not real entries — skip them so they never appear in listings.
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) continue;
        if (modified_files[i].is_tombstone) continue;

        if (count == index) {
            size_t plen = strlen(modified_files[i].path);
            if (plen >= sizeof(entry->name)) plen = sizeof(entry->name) - 1;
            memcpy(entry->name, modified_files[i].path, plen);
            entry->name[plen] = '\0';

            mode_t mbits = modified_files[i].mode & 0xF000;
            if (mbits == 0x4000) entry->type = FT_DIRECTORY;     // S_IFDIR
            else if (mbits == 0xA000) entry->type = FT_SYMLINK;  // S_IFLNK
            else entry->type = FT_FILE;                            // S_IFREG etc.

            return 0;
        }
        count++;
    }

    return -1; // Index out of range
}

int next_initrd_child(int *index, const char *dir_norm, char *child_name, size_t child_name_size, uint8_t *child_type) {
    char prefix[258];
    strncpy(prefix, dir_norm, sizeof(prefix) - 2);
    prefix[sizeof(prefix) - 2] = '\0';
    if (strcmp(dir_norm, "/") != 0) strcat(prefix, "/");
    size_t prefix_len = strlen(prefix);

    // Phase 1: archive entries.
    if (*index < archive_entry_count && archive_entries) {
        for (int i = *index; i < archive_entry_count; i++) {
            struct initrd_archive_entry *archive = &archive_entries[i];
            const char *name = archive->path;
            if (name[0] == '.' && name[1] == '/') name += 2;

            char abs_entry[256];
            abs_entry[0] = '/';
            strncpy(abs_entry + 1, name, sizeof(abs_entry) - 2);
            abs_entry[sizeof(abs_entry) - 1] = '\0';

            // Strip trailing slash on directory entries (./etc/ -> etc)
            // so the "no '/' in child name" direct-child test below
            // doesn't reject directories outright.
            size_t plen = strlen(abs_entry);
            if (plen > 1 && abs_entry[plen - 1] == '/') abs_entry[--plen] = '\0';

            if (strncmp(abs_entry, prefix, prefix_len) == 0) {
                const char *child = abs_entry + prefix_len;
                if (*child && !strchr(child, '/') &&
                    strcmp(child, ".") != 0 && strcmp(child, "..") != 0) {

                    if (!archive_tombstone_bits || !archive_tombstone_bits[i]) {
                        strncpy(child_name, child, child_name_size - 1);
                        child_name[child_name_size - 1] = '\0';
                        mode_t type = archive->mode & 0xF000;
                        *child_type = (type == 0x4000) ? DT_DIR :
                                      (type == 0xA000) ? DT_LNK : DT_REG;
                        *index = i;
                        return 0;
                    }
                }
            }

        }
        *index = archive_entry_count;
    }

    // Phase 2: overlay entries
    int overlay_start = (*index >= archive_entry_count) ? *index - archive_entry_count : 0;
    for (int i = overlay_start; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active || modified_files[i].is_tombstone) continue;

        const char *path = modified_files[i].path;
        if (path[0] == '.' && path[1] == '/') path += 2;

        char abs_entry[256];
        abs_entry[0] = '/';
        strncpy(abs_entry + 1, path, sizeof(abs_entry) - 2);
        abs_entry[sizeof(abs_entry) - 1] = '\0';

        size_t plen = strlen(abs_entry);
        if (plen > 1 && abs_entry[plen - 1] == '/') abs_entry[--plen] = '\0';

        if (strncmp(abs_entry, prefix, prefix_len) == 0) {
            const char *child = abs_entry + prefix_len;
            if (*child && !strchr(child, '/') &&
                strcmp(child, ".") != 0 && strcmp(child, "..") != 0) {

                strncpy(child_name, child, child_name_size - 1);
                child_name[child_name_size - 1] = '\0';
                mode_t mbits = modified_files[i].mode & 0xF000;
                *child_type = (mbits == 0x4000) ? DT_DIR :
                              (mbits == 0xA000) ? DT_LNK : DT_REG;
                *index = archive_entry_count + i;
                return 0;
            }
        }
    }

    return 1; // no more children
}

static bool parse_cpio_hex(const uint8_t *field, uint32_t *value) {
    uint32_t out = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = field[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else return false;
        out = (out << 4) | digit;
    }
    *value = out;
    return true;
}

static uint8_t *align_cpio(uint8_t *ptr, uint8_t *end) {
    uintptr_t aligned = ((uintptr_t)ptr + 3) & ~(uintptr_t)3;
    return aligned <= (uintptr_t)end ? (uint8_t *)aligned : NULL;
}

void init_initrd(void) {
    if (mod_req.response && mod_req.response->module_count > 0) {
        struct limine_file *mod = mod_req.response->modules[0];
        uint8_t *gz_data = (uint8_t *)mod->address;
        uint64_t gz_size = mod->size;

        if (gz_size < 18) panic("invalid initrd gzip module");
        // Read original size from last 4 bytes of gzip stream.
        uint32_t orig_size = *(uint32_t *)(gz_data + gz_size - 4);

        uint8_t *decompressed = malloc(orig_size);
        if (!decompressed) panic("memory allocation failed");

        int result = ungzip(gz_data, decompressed);
        if (result < 0) { free(decompressed); return; }

        uint8_t *end = decompressed + result;
        uint8_t *ptr = decompressed;
        int count = 0;
        while (ptr && ptr + 110 <= end) {
            if (memcmp(ptr, "070701", 6) != 0 && memcmp(ptr, "070702", 6) != 0) {
                panic("invalid newc initrd header"); free(decompressed); return;
            }
            uint32_t namesize, filesize;
            if (!parse_cpio_hex(ptr + 94, &namesize) || !parse_cpio_hex(ptr + 54, &filesize) ||
                namesize == 0 || namesize > 256) {
                panic("invalid newc initrd entry"); free(decompressed); return;
            }
            uint8_t *name = ptr + 110;
            if (name + namesize > end || name[namesize - 1] != '\0') {
                panic("truncated newc initrd entry"); free(decompressed); return;
            }
            if (strcmp((char *)name, "TRAILER!!!") == 0) break;
            uint8_t *data = align_cpio(name + namesize, end);
            if (!data || data + filesize > end) { panic("truncated newc initrd data"); free(decompressed); return; }
            ptr = align_cpio(data + filesize, end);
            if (!ptr) { panic("invalid newc initrd alignment"); free(decompressed); return; }
            count++;
        }
        if (ptr == NULL || ptr + 110 > end) { panic("missing newc initrd trailer"); free(decompressed); return; }

        archive_entries = malloc((size_t)count * sizeof(*archive_entries));
        if (!archive_entries && count) { panic("memory allocation failed"); free(decompressed); return; }
        memset(archive_entries, 0, (size_t)count * sizeof(*archive_entries));

        ptr = decompressed;
        for (int i = 0; i < count; i++) {
            uint32_t namesize, filesize;
            parse_cpio_hex(ptr + 94, &namesize); parse_cpio_hex(ptr + 54, &filesize);
            uint8_t *name = ptr + 110;
            uint8_t *data = align_cpio(name + namesize, end);
            struct initrd_archive_entry *entry = &archive_entries[i];
            strncpy(entry->path, (char *)name, sizeof(entry->path) - 1);
            if (strcmp(entry->path, ".") == 0) strcpy(entry->path, "./");
            parse_cpio_hex(ptr + 14, (uint32_t *)&entry->mode);
            parse_cpio_hex(ptr + 22, (uint32_t *)&entry->uid);
            parse_cpio_hex(ptr + 30, (uint32_t *)&entry->gid);
            parse_cpio_hex(ptr + 6, &entry->ino);
            parse_cpio_hex(ptr + 38, &entry->nlink);
            parse_cpio_hex(ptr + 62, &entry->devmajor);
            parse_cpio_hex(ptr + 70, &entry->devminor);
            entry->data = data;
            entry->size = filesize;
            if ((entry->mode & 0xF000) == 0xA000) {
                size_t target_size = filesize < sizeof(entry->link_target) - 1 ? filesize : sizeof(entry->link_target) - 1;
                memcpy(entry->link_target, data, target_size);
                entry->link_target[target_size] = '\0';
                entry->data = (uint8_t *)entry->link_target;
                entry->size = target_size;
            }
            ptr = align_cpio(data + filesize, end);
        }

        // newc represents hard links by repeated (inode, device) tuples and
        // may place the only payload on a later entry.
        for (int i = 0; i < count; i++) {
            struct initrd_archive_entry *entry = &archive_entries[i];
            if ((entry->mode & 0xF000) != 0x8000 || entry->nlink < 2) continue;
            for (int j = 0; j < count; j++) {
                struct initrd_archive_entry *peer = &archive_entries[j];
                if (peer->ino == entry->ino && peer->devmajor == entry->devmajor &&
                    peer->devminor == entry->devminor && peer->size > 0) {
                    entry->data = peer->data;
                    entry->size = peer->size;
                    break;
                }
            }
        }

        archive_entry_count = count;
        quicksort_archive_entries(archive_entries, 0, archive_entry_count - 1);
        archive_tombstone_bits = malloc((size_t)archive_entry_count);
        if (archive_tombstone_bits) memset(archive_tombstone_bits, 0, (size_t)archive_entry_count);
        initrd_decompressed = decompressed;

        // Reclaim the compressed module memory: from this point on we only
        // ever consult the decompressed buffer + archive_entries, so the
        // original gzip blob is dead weight. Limine reserved it as
        // KERNEL_AND_MODULES and the PMM never marked it free, so punch those
        // pages back into the allocator manually.  This returns the entire
        // compressed size (~tens of MB on a full rootfs) to the system.
        pfree_range((void *)mod->address, mod->size);
    } else {
        panic("no module found");
    }
    log("initialized initrd");
}
