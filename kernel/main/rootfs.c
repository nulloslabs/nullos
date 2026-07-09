#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <freestanding/sys/stat.h>
#include <freestanding/sys/types.h>
#include <main/panic.h>
#include <io/terminal.h>
#include <main/rootfs.h>
#include <main/string.h>
#include <main/gzip.h>
#include <mm/mm.h>
#include <freestanding/errno.h>
#include <freestanding/dirent.h>
#include <main/sched.h>
#include <main/limine_req.h>
#include <limine/limine.h>

static uint8_t *tar_archive_start = NULL;
static uint8_t *tar_decompressed = NULL;
static uint8_t **tar_entry_ptrs = NULL;
static int tar_total_count = 0;

// Tombstone bitmap: one byte per SORTED tar entry (indexed the same way as
// tar_entry_ptrs[]). When a tar-backed path is deleted (unlink/rmdir), the
// byte at its sorted index is set to 1 so subsequent read/stat/getdents calls
// hide the original. This removes the old MAX_MODIFIED_FILES-based tombstone
// storage, which exhausted and caused "rm -rf /" to fail with ENOTEMPTY once
// the overlay table filled up.
static uint8_t *tar_tombstone_bits = NULL;

// struct tar_symlink moved to rootfs.h so other translation units (and
// future debug tooling) can inspect the symlink table without re-declaring it.
static struct tar_symlink *tar_symlinks = NULL;
static int tar_symlink_count = 0;
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

static uint64_t parse_octal(const char *str) {
    uint64_t val = 0;
    while (*str >= '0' && *str <= '7') {
        val = (val << 3) | (*str - '0');
        str++;
    }
    return val;
}

static int compare_tar_names(const char *a, const char *b) {
    int i = 0;
    while (i < 100) {
        char c1 = a[i];
        if (c1 == '/' && (i == 99 || a[i+1] == '\0' || a[i+1] == '/')) c1 = '\0';
        char c2 = b[i];
        if (c2 == '/' && (i == 99 || b[i+1] == '\0' || b[i+1] == '/')) c2 = '\0';
        
        if (c1 == '\0' && c2 == '\0') return 0;
        if (c1 == '\0') return -1;
        if (c2 == '\0') return 1;
        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
        i++;
    }
    return 0;
}

static void swap_ptrs(uint8_t **a, uint8_t **b) {
    uint8_t *t = *a; *a = *b; *b = t;
}

static void quicksort_tar_ptrs(uint8_t **arr, int low, int high) {
    if (low < high) {
        struct tar_header *pivot = (struct tar_header *)arr[high];
        int i = low - 1;
        for (int j = low; j < high; j++) {
            struct tar_header *h = (struct tar_header *)arr[j];
            if (compare_tar_names(h->name, pivot->name) < 0) {
                i++;
                swap_ptrs(&arr[i], &arr[j]);
            }
        }
        swap_ptrs(&arr[i + 1], &arr[high]);
        int pi = i + 1;
        quicksort_tar_ptrs(arr, low, pi - 1);
        quicksort_tar_ptrs(arr, pi + 1, high);
    }
}

// Binary-search the sorted tar_entry_ptrs[] for the index of `norm_path`.
// Returns the sorted index, or -1 if not found. Used by every tar lookup site
// to consult the tombstone bitmap in O(log N).
static int tar_tombstone_idx(const char *norm_path) {
    if (!tar_entry_ptrs || tar_total_count <= 0) return -1;
    int low = 0, high = tar_total_count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        struct tar_header *h = (struct tar_header *)tar_entry_ptrs[mid];
        int cmp = compare_tar_names(h->name, norm_path);
        if (cmp == 0) return mid;
        if (cmp < 0) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

static bool tar_tombstone_get(const char *norm_path) {
    int idx = tar_tombstone_idx(norm_path);
    if (idx < 0 || !tar_tombstone_bits) return false;
    return tar_tombstone_bits[idx] != 0;
}

static void tar_tombstone_set(const char *norm_path) {
    int idx = tar_tombstone_idx(norm_path);
    if (idx >= 0 && tar_tombstone_bits) tar_tombstone_bits[idx] = 1;
}

static void tar_tombstone_clear(const char *norm_path) {
    int idx = tar_tombstone_idx(norm_path);
    if (idx >= 0 && tar_tombstone_bits) tar_tombstone_bits[idx] = 0;
}

// Variant for the getdents walks that hold the tar header's own name (which
// may carry a trailing slash on directories) plus its precomputed length.
// Strips the trailing slash then does the bitmap lookup.
static bool tar_tombstone_get_n(const char *tar_name, size_t len) {
    char buf[128];
    while (len > 0 && tar_name[len - 1] == '/') len--;
    if (len == 0) return false;
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, tar_name, n);
    buf[n] = '\0';
    return tar_tombstone_get(buf);
}

static bool find_tar_symlink(const char *abs_path, char *resolved_abs, size_t resolved_size) {
    char norm[256];
    normalize_path(abs_path, norm, sizeof(norm));

    // Check overlay symlinks first (created via symlink_rootfs)
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && !modified_files[i].is_tombstone &&
            (modified_files[i].mode & 0xF000) == 0xA000 &&
            compare_tar_names(modified_files[i].path, norm) == 0) {
            char target[101];
            strncpy(target, (const char *)modified_files[i].data, 100);
            target[100] = '\0';
            resolve_link_target(abs_path, target, resolved_abs, resolved_size);
            return true;
        }
    }

    // Fall back to tar symlinks. Skip any whose tar entry has been
    // tombstoned (deleted) — they must not be followed during path resolution,
    // or a deleted symlink would still redirect lookups to its old target.
    if (!tar_symlinks) return false;

    for (int i = 0; i < tar_symlink_count; i++) {
        if (compare_tar_names(tar_symlinks[i].path, norm) == 0) {
            if (tar_tombstone_get(tar_symlinks[i].path)) return false;
            strncpy(resolved_abs, tar_symlinks[i].target, resolved_size - 1);
            resolved_abs[resolved_size - 1] = '\0';
            return true;
        }
    }

    return false;
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
            if (find_tar_symlink(prefix, target, sizeof(target))) {
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
            // tar_tombstone_get_n / tar_tombstone_get).
            tar_tombstone_clear(path);
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
            tar_tombstone_clear(path);
            return;
        }
    }
}

// Drop a tombstone: mark the SORTED tar entry for `norm_path` (already in
// "./foo/bar" form) as deleted so read/stat/getdents hide the original.
// Uses the per-entry bitmap instead of the overlay array, so there is no
// MAX_MODIFIED_FILES ceiling and "rm -rf /" no longer exhausts the table.
static void add_tombstone(const char *norm_path) {
    tar_tombstone_set(norm_path);
}

// Returns true if the sorted tar index table contains `norm_path`.
// Uses the binary-search index (O(log n)) instead of the old linear walk.
static bool tar_has_entry(const char *norm_path) {
    return tar_tombstone_idx(norm_path) >= 0;
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

rootfs_file_t read_rootfs(const char *path) {
    char current_path[256];
    strncpy(current_path, path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    rootfs_file_t result = { .data = NULL, .size = 0, .mode = 0, .uid = 0, .gid = 0 };

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

        // If the tar entry beneath us is tombstoned, report not found.
        if (tar_tombstone_get(norm)) return result;

        // 2. Check TAR archive
        if (tar_archive_start != NULL) {
            if (tar_entry_ptrs) {
                int low = 0, high = tar_total_count - 1;
                while (low <= high) {
                    int mid = low + (high - low) / 2;
                    struct tar_header *h = (struct tar_header *)tar_entry_ptrs[mid];
                    int cmp = compare_tar_names(h->name, norm);
                    if (cmp == 0) {
                        char type = h->typeflag[0];
                        if (type == '1' || type == '2') {
                            char target[101];
                            strncpy(target, h->linkname, 100);
                            target[100] = '\0';
                            char current_abs[256];
                            get_absolute_path(current_path, current_abs, sizeof(current_abs));
                            resolve_link_target(current_abs, target, current_path, sizeof(current_path));
                            found_link = true;
                            break;
                        }
                        if (type == '0' || type == '\0' || type == '5') {
                            result.size = (size_t)parse_octal(h->size);
                            result.data = (void *)((uint8_t*)h + 512);
                            result.mode = (type == '5') ? ((uint32_t)parse_octal(h->mode) | 0040000) : ((uint32_t)parse_octal(h->mode) | 0100000);
                            result.uid = (uid_t)parse_octal(h->uid);
                            result.gid = (gid_t)parse_octal(h->gid);
                            return result;
                        }
                        break;
                    } else if (cmp < 0) {
                        low = mid + 1;
                    } else {
                        high = mid - 1;
                    }
                }
            } else {
                uint8_t *ptr = tar_archive_start;
                while (1) {
                    struct tar_header *h = (struct tar_header *)ptr;
                    if (h->name[0] == '\0') break;

                    uint64_t size = parse_octal(h->size);
                    if (compare_tar_names(h->name, norm) == 0) {
                        char type = h->typeflag[0];
                        if (type == '1' || type == '2') {
                            char target[101];
                            strncpy(target, h->linkname, 100);
                            target[100] = '\0';
                            char current_abs[256];
                            get_absolute_path(current_path, current_abs, sizeof(current_abs));
                            resolve_link_target(current_abs, target, current_path, sizeof(current_path));
                            found_link = true;
                            break;
                        }
                        if (type == '0' || type == '\0' || type == '5') {
                            result.size = size;
                            result.data = ptr + 512;
                            result.mode = (type == '5') ? ((uint32_t)parse_octal(h->mode) | 0040000) : ((uint32_t)parse_octal(h->mode) | 0100000);
                            result.uid = (uid_t)parse_octal(h->uid);
                            result.gid = (gid_t)parse_octal(h->gid);
                            return result;
                        }
                    }
                    ptr += 512 + (size + 511) / 512 * 512;
                }
            }
        }
        if (!found_link) break;
    }

    return result;
}

static rootfs_file_t stat_rootfs_ex(const char *path, bool follow_final) {
    char norm[256];
    // Normalizes the path and resolves directory symlinks in the prefix,
    // optionally resolving the final component too (follow_final).
    get_norm_path_ex(path, norm, sizeof(norm), follow_final);

    // check overlay first
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            rootfs_file_t result = {
                .data = modified_files[i].data,
                .size = modified_files[i].size,
                .mode = modified_files[i].mode,
                .uid  = modified_files[i].uid,
                .gid  = modified_files[i].gid,
            };
            return result;
        }
    }

    // Tombstoned tar entry: deleted.
    if (tar_tombstone_get(norm)) {
        rootfs_file_t empty = {0};
        return empty;
    }

    // check TAR, return the entry as-is (no final-component following here)
    if (tar_archive_start) {
        if (tar_entry_ptrs) {
            int low = 0, high = tar_total_count - 1;
            while (low <= high) {
                int mid = low + (high - low) / 2;
                struct tar_header *h = (struct tar_header *)tar_entry_ptrs[mid];
                int cmp = compare_tar_names(h->name, norm);
                if (cmp == 0) {
                    uint32_t mode = (uint32_t)parse_octal(h->mode);
                    char type = h->typeflag[0];
                    switch (type) {
                        case '0': case '\0': mode |= 0100000; break; // S_IFREG
                        case '2':            mode |= 0120000; break; // S_IFLNK
                        case '5':            mode |= 0040000; break; // S_IFDIR
                        case '3':            mode |= 0020000; break; // S_IFCHR
                        case '4':            mode |= 0060000; break; // S_IFBLK
                        case '6':            mode |= 0010000; break; // S_IFIFO
                    }
                    void *data_ptr = (type == '2') ? (void *)h->linkname : (void *)((uint8_t*)h + 512);
                    size_t data_size = (type == '2') ? strnlen(h->linkname, 100) : (size_t)parse_octal(h->size);
                    rootfs_file_t result = {
                        .data = data_ptr,
                        .size = data_size,
                        .mode = mode,
                        .uid  = (uid_t)parse_octal(h->uid),
                        .gid  = (gid_t)parse_octal(h->gid),
                    };
                    return result;
                } else if (cmp < 0) {
                    low = mid + 1;
                } else {
                    high = mid - 1;
                }
            }
        } else {
            uint8_t *ptr = tar_archive_start;
            while (1) {
                struct tar_header *h = (struct tar_header *)ptr;
                if (h->name[0] == '\0') break;
                uint64_t size = parse_octal(h->size);
                if (compare_tar_names(h->name, norm) == 0) {
                    uint32_t mode = (uint32_t)parse_octal(h->mode);
                    char type = h->typeflag[0];
                    switch (type) {
                        case '0': case '\0': mode |= 0100000; break; // S_IFREG
                        case '2':            mode |= 0120000; break; // S_IFLNK
                        case '5':            mode |= 0040000; break; // S_IFDIR
                        case '3':            mode |= 0020000; break; // S_IFCHR
                        case '4':            mode |= 0060000; break; // S_IFBLK
                        case '6':            mode |= 0010000; break; // S_IFIFO
                    }
                    void *data_ptr = (type == '2') ? (void *)h->linkname : (void *)(ptr + 512);
                    size_t data_size = (type == '2') ? strnlen(h->linkname, 100) : (size_t)size;
                    rootfs_file_t result = {
                        .data = data_ptr,
                        .size = data_size,
                        .mode = mode,
                        .uid  = (uid_t)parse_octal(h->uid),
                        .gid  = (gid_t)parse_octal(h->gid),
                    };
                    return result;
                }
                ptr += 512 + (size + 511) / 512 * 512;
            }
        }
    }

    rootfs_file_t empty = {0};
    return empty;
}

rootfs_file_t stat_rootfs(const char *path) {
    return stat_rootfs_ex(path, true);
}

rootfs_file_t stat_rootfs_nofollow(const char *path) {
    size_t len = strlen(path);
    bool follow_final = false;
    if (len > 0 && path[len - 1] == '/') {
        follow_final = true;
    }
    return stat_rootfs_ex(path, follow_final);
}

int write_rootfs(const char *path, const void *data, uint64_t size, uint32_t mode, uid_t uid, gid_t gid) {
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
// write_rootfs() above takes a full-replacement buffer and malloc+memcpy of
// the whole file each call, which makes dd-style loops (many small writes
// to a growing file) O(N^2) — a 1.5 MiB image via `dd bs=512` took ~35 s.
//
// This helper reuses the existing overlay buffer when one exists: it only
// reallocates (geometrically, 1.5x) when the write extends past the current
// allocation, and copies only the newly-written bytes.  Each write is thus
// O(off+count) amortized, not O(file_size).  When no overlay entry exists
// yet (first write on a tar-backed or brand-new file), it snapshots the
// current contents, applies the patch, and stores a fresh overlay entry.
int write_rootfs_partial(const char *path, const void *data, uint64_t off, uint64_t count, uint32_t mode, uid_t uid, gid_t gid) {
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
    rootfs_file_t cur = read_rootfs(path);
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

int mkdir_rootfs(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    add_modified_file(norm, NULL, 0, 0, mode | 0040000, uid, gid);
    return 0;
}


int delete_rootfs(const char *path) {
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
            // If a tar original also exists below it, leave a tombstone so it
            // doesn't resurface. Otherwise there's nothing to shadow.
            if (tar_has_entry(norm)) add_tombstone(norm);
            return 0;
        }
    }

    // Not in the overlay — but if the tar archive has it, record a tombstone so
    // the original is hidden from read/stat/getdents from now on.
    if (tar_has_entry(norm)) {
        add_tombstone(norm);
        return 0;
    }

    return -1; // Not found
}

int rmdir_rootfs(const char *path) {
    // Don't follow the final component: rmdir on a symlink should fail with
    // ENOTDIR, not remove the target directory.
    char norm[256];
    get_norm_path_ex(path, norm, sizeof(norm), false);

    // check it exists and is a directory
    rootfs_file_t file = stat_rootfs(path);
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

    // check TAR for any children. Use the SORTED pointer array so we can
    // binary-search for the first entry >= "<norm>/" and then walk forward
    // only as long as the prefix matches — O(log N + matches) instead of a
    // full O(N) linear scan per rmdir() call. This is what made `rm -rf`
    // slow: every directory removal re-walked the whole 6k-entry archive.
    if (tar_entry_ptrs && tar_total_count > 0) {
        // Build "<norm>/" as the lower-bound search key. compare_tar_names
        // treats a trailing '/' as a terminator, so this finds the first
        // entry whose name sorts at or after the directory's children.
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

        int low = 0, high = tar_total_count - 1, first = tar_total_count;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            struct tar_header *h = (struct tar_header *)tar_entry_ptrs[mid];
            if (compare_tar_names(h->name, key) < 0) low = mid + 1;
            else { first = mid; high = mid - 1; }
        }

        // Walk forward from `first` while the entry is a child of <norm>.
        // Because the array is sorted, once we pass the children range we
        // can stop immediately — no need to scan the rest of the archive.
        for (int i = first; i < tar_total_count; i++) {
            struct tar_header *h = (struct tar_header *)tar_entry_ptrs[i];
            // If this entry doesn't start with "<norm>/", we've passed the
            // children block (sorted order) — stop scanning.
            if (strncmp(h->name, norm, norm_len) != 0 ||
                h->name[norm_len] != '/') break;

            // Skip tombstoned (deleted) children — they're invisible.
            if (tar_tombstone_bits && tar_tombstone_bits[i]) continue;

            // Direct child: directory is not empty.
            if (strlen(h->name) > norm_len + 1) return -ENOTEMPTY;
        }
    }

    // Remove it. If it's an overlay entry, drop the entry and leave a tombstone
    // if a tar original sits below it (so it doesn't resurface). If it's
    // tar-backed only, a tombstone is the only way to hide the original.
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            if (modified_files[i].data) free(modified_files[i].data);
            modified_files[i].is_active = false;
            if (tar_has_entry(norm)) add_tombstone(norm);
            return 0;
        }
    }

    if (tar_has_entry(norm)) {
        add_tombstone(norm);
        return 0;
    }

    return -ENOENT;
}

int symlink_rootfs(const char *target, const char *path, uid_t uid, gid_t gid) {
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

int chmod_rootfs(const char *path, mode_t mode) {
    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    // Search for the entry in the overlay and update its mode in-place.
    // We must NOT call read_rootfs + add_modified_file here because
    // read_rootfs returns the same data pointer stored in the overlay,
    // and add_modified_file would free(old_data) then store the
    // now-dangling pointer back — use-after-free.
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            mode_t type_bits = (modified_files[i].mode & 0xF000);
            modified_files[i].mode = (mode & 0777) | type_bits;
            return 0;
        }
    }

    // Entry not in overlay — check the tar archive and promote it to
    // the overlay so the new mode persists.
    rootfs_file_t file = read_rootfs(path);
    if (!file.mode) { return -ENOENT; }

    mode_t type_bits = (file.mode & 0xF000);
    mode_t new_mode = (mode & 0777) | type_bits;

    // file.data from the tar archive is not owned by the overlay, so
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

int get_rootfs_entry(int index, directory_entry_t *entry) {
    if (!entry) return -1;

    int count = 0;

    // Phase 1: tar archive entries, skipping any the overlay shadows.
    if (tar_archive_start) {
        uint8_t *ptr = tar_archive_start;
        while (1) {
            struct tar_header *h = (struct tar_header *)ptr;
            if (h->name[0] == '\0') break;

            uint64_t size = parse_octal(h->size);
            char type = h->typeflag[0];

            // Return the full tar path (e.g. "./usr/bin/sh") so the caller can
            // do directory-prefix filtering. Strip a trailing slash for dirs.
            size_t nlen = strlen(h->name);
            if (nlen > 1 && h->name[nlen - 1] == '/') nlen--;

            if (!tar_tombstone_get_n(h->name, nlen)) {
                if (count == index) {
                    if (nlen >= sizeof(entry->name)) nlen = sizeof(entry->name) - 1;
                    memcpy(entry->name, h->name, nlen);
                    entry->name[nlen] = '\0';

                    if (type == '5') entry->type = FT_DIRECTORY;
                    else if (type == '2') entry->type = FT_SYMLINK;
                    else entry->type = FT_FILE;  // '0', '\0', or anything else

                    return 0;
                }
                count++;
            }

            ptr += 512 + (size + 511) / 512 * 512;
        }
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

int next_rootfs_child(int *index, const char *dir_norm, char *child_name, size_t child_name_size, uint8_t *child_type) {
    char prefix[258];
    strncpy(prefix, dir_norm, sizeof(prefix) - 2);
    prefix[sizeof(prefix) - 2] = '\0';
    if (strcmp(dir_norm, "/") != 0) strcat(prefix, "/");
    size_t prefix_len = strlen(prefix);

    // Phase 1: tar entries — sequential walk using pre-computed pointer array
    if (*index < tar_total_count && tar_entry_ptrs) {
        for (int i = *index; i < tar_total_count; i++) {
            struct tar_header *h = (struct tar_header *)tar_entry_ptrs[i];
            char type = h->typeflag[0];

            const char *tar_name = h->name;
            size_t full_len = strlen(tar_name);
            if (full_len > 1 && tar_name[full_len - 1] == '/') full_len--;

            const char *name = tar_name;
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

                    if (!tar_tombstone_get_n(tar_name, full_len)) {
                        strncpy(child_name, child, child_name_size - 1);
                        child_name[child_name_size - 1] = '\0';
                        *child_type = (type == '5') ? DT_DIR :
                                      (type == '2') ? DT_LNK : DT_REG;
                        *index = i;
                        return 0;
                    }
                }
            }

            if (h->name[0] == '\0') break;
        }
        *index = tar_total_count; // tar exhausted, advance to overlay marker
    }

    // Phase 2: overlay entries
    int overlay_start = (*index >= tar_total_count) ? *index - tar_total_count : 0;
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
                *index = tar_total_count + i;
                return 0;
            }
        }
    }

    return 1; // no more children
}

void init_rootfs(void) {
    if (mod_req.response && mod_req.response->module_count > 0) {
        struct limine_file *mod = mod_req.response->modules[0];
        uint8_t *gz_data = (uint8_t *)mod->address;
        uint64_t gz_size = mod->size;

        // Read original size from last 4 bytes of gzip stream
        uint32_t orig_size = *(uint32_t *)(gz_data + gz_size - 4);

        uint8_t *decompressed = malloc(orig_size);
        if (!decompressed) { panic("memory allocation failed"); return; }

        int result = ungzip(gz_data, decompressed);
        if (result < 0) { free(decompressed); return; }

        tar_archive_start = decompressed;
        tar_decompressed = decompressed;

        uint8_t *ptr = decompressed;
        tar_total_count = 0;
        while (1) {
            struct tar_header *h = (struct tar_header *)ptr;
            if (h->name[0] == '\0') break;
            uint64_t size = parse_octal(h->size);
            char type = h->typeflag[0];
            if (type == '1' || type == '2') tar_symlink_count++;
            tar_total_count++;
            ptr += 512 + (size + 511) / 512 * 512;
        }

        tar_entry_ptrs = malloc(tar_total_count * sizeof(uint8_t *));
        if (tar_entry_ptrs) {
            ptr = decompressed;
            for (int i = 0; i < tar_total_count; i++) {
                tar_entry_ptrs[i] = ptr;
                struct tar_header *h = (struct tar_header *)ptr;
                uint64_t size = parse_octal(h->size);
                ptr += 512 + (size + 511) / 512 * 512;
            }
            quicksort_tar_ptrs(tar_entry_ptrs, 0, tar_total_count - 1);

            // Allocate the tombstone bitmap — one byte per sorted tar entry,
            // indexed identically to tar_entry_ptrs[] so the binary-search
            // index returned by tar_tombstone_idx() is a direct bitmap key.
            // This replaces the old overlay-array tombstone slots and removes
            // the MAX_MODIFIED_FILES ceiling that broke `rm -rf /`.
            tar_tombstone_bits = malloc((size_t)tar_total_count);
            if (tar_tombstone_bits) {
                memset(tar_tombstone_bits, 0, (size_t)tar_total_count);
            }
        } else {
            tar_total_count = 0;
        }

        if (tar_symlink_count > 0) {
            tar_symlinks = malloc(tar_symlink_count * sizeof(struct tar_symlink));
            int idx = 0;
            ptr = decompressed;
            while (1) {
                struct tar_header *h = (struct tar_header *)ptr;
                if (h->name[0] == '\0') break;
                uint64_t size = parse_octal(h->size);
                char type = h->typeflag[0];
                if (type == '1' || type == '2') {
                    strncpy(tar_symlinks[idx].path, h->name, 128);
                    strncpy(tar_symlinks[idx].target, h->linkname, 128);
                    idx++;
                }
                ptr += 512 + (size + 511) / 512 * 512;
            }
        }
    } else {
        panic("no module found");
    }
    printf("rootfs: initialized rootfs\n");
}
