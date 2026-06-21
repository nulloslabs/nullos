#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>
#include <main/panic.h>
#include <io/terminal.h>
#include <main/rootfs.h>
#include <main/string.h>
#include <main/gzip.h>
#include <mm/mm.h>
#include <freestanding/errno.h>
#include <main/scheduler.h>
#include <main/limine_req.h>
#include <limine/limine.h>

static uint8_t *tar_archive_start = NULL;
static uint8_t *tar_decompressed = NULL;
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

static bool tar_name_matches(const char *tar_name, const char *norm) {
    if (strcmp(tar_name, norm) == 0) return true;

    // Try matching tar_name with trailing slash stripped
    size_t tlen = strlen(tar_name);
    if (tlen > 1 && tar_name[tlen - 1] == '/') {
        // Compare tar_name without trailing slash vs norm
        if (strncmp(tar_name, norm, tlen - 1) == 0 && norm[tlen - 1] == '\0') return true;
    }

    // Try matching norm with trailing slash appended vs tar_name
    size_t nlen = strlen(norm);
    if (nlen > 0 && norm[nlen - 1] != '/') {
        char norm_slash[256];
        strncpy(norm_slash, norm, sizeof(norm_slash) - 2);
        norm_slash[sizeof(norm_slash) - 2] = '\0';
        strcat(norm_slash, "/");
        if (strcmp(tar_name, norm_slash) == 0) return true;
    }

    return false;
}

static bool find_tar_symlink(const char *abs_path, char *resolved_abs, size_t resolved_size) {
    if (!tar_archive_start) return false;

    char norm[256];
    normalize_path(abs_path, norm, sizeof(norm));

    uint8_t *ptr = tar_archive_start;
    while (1) {
        struct tar_header *h = (struct tar_header *)ptr;
        if (h->name[0] == '\0') break;

        uint64_t size = parse_octal(h->size);
        char type = h->typeflag[0];

        if ((type == '1' || type == '2') && tar_name_matches(h->name, norm)) {
            char target[101];
            strncpy(target, h->linkname, 100);
            target[100] = '\0';
            resolve_link_target(abs_path, target, resolved_abs, resolved_size);
            return true;
        }

        ptr += 512 + (size + 511) / 512 * 512;
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

static void add_modified_file(const char *path, void *data, size_t size, uint32_t mode, uid_t uid, gid_t gid) {
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, path) == 0) {
            if (modified_files[i].data) free(modified_files[i].data);
            modified_files[i].data = data;
            modified_files[i].size = size;
            modified_files[i].mode = mode;
            modified_files[i].uid = uid;
            modified_files[i].gid = gid;
            modified_files[i].is_tombstone = false;
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
            modified_files[i].mode = mode;
            modified_files[i].uid = uid;
            modified_files[i].gid = gid;
            modified_files[i].is_tombstone = false;
            return;
        }
    }
}

// Drop a tombstone: mark `norm_path` (already in "./foo/bar" form) as deleted
// so the tar archive entry beneath it is hidden. If an overlay entry exists at
// that path, the caller has already removed it (or never had one); a tombstone
// is recorded regardless so the tar original — if any — stays gone.
static void add_tombstone(const char *norm_path) {
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm_path) == 0) {
            if (modified_files[i].data) free(modified_files[i].data);
            modified_files[i].data = NULL;
            modified_files[i].size = 0;
            modified_files[i].mode = 0;
            modified_files[i].is_tombstone = true;
            return;
        }
    }

    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) {
            modified_files[i].is_active = true;
            strncpy(modified_files[i].path, norm_path, sizeof(modified_files[i].path) - 1);
            modified_files[i].path[sizeof(modified_files[i].path) - 1] = '\0';
            modified_files[i].data = NULL;
            modified_files[i].size = 0;
            modified_files[i].mode = 0;
            modified_files[i].is_tombstone = true;
            return;
        }
    }
}

// Does the tar archive contain any entry (file/dir/symlink) at `norm_path`
// (already in "./foo/bar" form)? Used to decide whether a deletion needs a
// tombstone to keep the original from resurfacing.
static bool tar_has_entry(const char *norm_path) {
    if (!tar_archive_start) return false;
    uint8_t *ptr = tar_archive_start;
    while (1) {
        struct tar_header *h = (struct tar_header *)ptr;
        if (h->name[0] == '\0') break;
        uint64_t size = parse_octal(h->size);
        if (tar_name_matches(h->name, norm_path)) return true;
        ptr += 512 + (size + 511) / 512 * 512;
    }
    return false;
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

        // 1. Check overlay
        bool found_link = false;
        for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
            if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
                // Tombstone: this path was deleted (it shadowed a tar entry).
                // Stop here and report "not found" — do NOT fall through to
                // the tar archive, or the deleted file would reappear.
                if (modified_files[i].is_tombstone) {
                    return result;
                }
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

        // 2. Check TAR archive
        if (tar_archive_start != NULL) {
            uint8_t *ptr = tar_archive_start;
            while (1) {
                struct tar_header *h = (struct tar_header *)ptr;
                if (h->name[0] == '\0') break;

                uint64_t size = parse_octal(h->size);
                if (tar_name_matches(h->name, norm)) {
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
            // Tombstone: deleted. Report as missing; don't expose the tar
            // original hiding underneath.
            if (modified_files[i].is_tombstone) {
                rootfs_file_t empty = {0};
                return empty;
            }
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

    // check TAR, return the entry as-is (no final-component following here)
    if (tar_archive_start) {
        uint8_t *ptr = tar_archive_start;
        while (1) {
            struct tar_header *h = (struct tar_header *)ptr;
            if (h->name[0] == '\0') break;
            uint64_t size = parse_octal(h->size);
            if (tar_name_matches(h->name, norm)) {
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
                // Symlinks carry their target in the linkname header field,
                // not in a data section (their size is 0). Point file.data at
                // h->linkname so readlink() returns the right string.
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

    add_modified_file(norm, copy, (size_t)size, mode, uid, gid);
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

int mkdir_rootfs(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char norm[256];
    get_norm_path(path, norm, sizeof(norm));

    add_modified_file(norm, NULL, 0, mode | 0x4000, uid, gid); // S_IFDIR
    return 0;
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

    // check it's empty — scan modified_files for any children
    size_t norm_len = strlen(norm);
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) continue;
        if (strcmp(modified_files[i].path, norm) == 0) continue; // itself
        if (strncmp(modified_files[i].path, norm, norm_len) == 0 &&
            modified_files[i].path[norm_len] == '/') return -ENOTEMPTY;
    }

    // check TAR for any children
    if (tar_archive_start) {
        uint8_t *ptr = tar_archive_start;
        while (1) {
            struct tar_header *h = (struct tar_header *)ptr;
            if (h->name[0] == '\0') break;
            uint64_t size = parse_octal(h->size);
            if (strncmp(h->name, norm, norm_len) == 0 &&
                h->name[norm_len] == '/' &&
                strlen(h->name) > norm_len + 1) return -ENOTEMPTY;
            ptr += 512 + (size + 511) / 512 * 512;
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

    add_modified_file(norm, copy, len, 0xA000 | 0777, uid, gid); // S_IFLNK
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

    add_modified_file(norm, data_copy, file.size, new_mode, file.uid, file.gid);
    return 0;
}

// Returns true if the overlay (modified_files[]) holds any entry at the given
// normalized path — including a tombstone. `norm_path`/`len` are the
// "./foo/bar" form used throughout rootfs, without a trailing slash. Used to
// skip tar entries during directory enumeration: a real overlay entry would
// otherwise be listed twice (once per source), and a tombstoned entry must
// not be listed at all.
static bool overlay_has_path_n(const char *norm_path, size_t len) {
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) continue;
        const char *p = modified_files[i].path;
        if (strlen(p) == len && strncmp(p, norm_path, len) == 0)
            return true;
    }
    return false;
}

// Enumerate a merged view of the rootfs: every tar entry that isn't shadowed
// by an overlay entry, followed by every overlay entry. Without this merge,
// files/dirs created at runtime (mkdir, write, symlink — which land in the
// modified_files overlay) never show up in directory listings, so getdents()
// of a directory containing them comes back empty (e.g. `ls /` after
// `cp -r /etc /etc-copy` would not list /etc-copy).
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

            if (!overlay_has_path_n(h->name, nlen)) {
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
    } else {
        panic("no module found");
    }
    printf("rootfs: initialized rootfs\n");
}
