#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <freestanding/sys/types.h>
#include <main/panic.h>
#include <io/terminal.h>
#include <main/rootfs.h>
#include <main/string.h>
#include <main/gzip.h>
#include <mm/mm.h>
#include <main/errno.h>
#include <main/scheduler.h>
#include <main/limine_req.h>
#include <limine/limine.h>

#define MAX_MODIFIED_FILES 128

static uint8_t *tar_archive_start = NULL;
static uint8_t *tar_decompressed = NULL;
static modified_file_t modified_files[MAX_MODIFIED_FILES];

static void get_absolute_path(const char *in, char *out_abs, size_t out_size) {
    if (in[0] == '/') {
        strncpy(out_abs, in, out_size - 1);
    } else {
        if (current_task_ptr) {
            strncpy(out_abs, current_task_ptr->cwd, out_size - 1);
            if (strcmp(out_abs, "/") != 0)
                strncat(out_abs, "/", out_size - strlen(out_abs) - 1);
        } else {
            strcpy(out_abs, "/");
        }
        strncat(out_abs, in, out_size - strlen(out_abs) - 1);
    }
    out_abs[out_size - 1] = '\0';
}

static void collapse_slashes(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    bool last_was_slash = false;

    for (size_t i = 0; in[i] && j + 1 < out_size; i++) {
        if (in[i] == '/') {
            if (last_was_slash) continue;
            last_was_slash = true;
        } else {
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
        if (strcmp(start, "..") == 0) {
            if (depth > 0) depth--;
            continue;
        }
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

static void resolve_link_target(const char *base_path_abs, const char *link_target, char *out_abs, size_t out_size) {
    if (link_target[0] == '/') {
        strncpy(out_abs, link_target, out_size - 1);
        out_abs[out_size - 1] = '\0';
        return;
    }
    strncpy(out_abs, base_path_abs, out_size - 1);
    out_abs[out_size - 1] = '\0';

    char *last_slash = strrchr(out_abs, '/');
    if (last_slash) {
        last_slash[1] = '\0';
        strncat(out_abs, link_target, out_size - strlen(out_abs) - 1);
    } else {
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

static void resolve_path_symlinks(const char *in_abs, char *out_abs, size_t out_size) {
    char current[256];
    strncpy(current, in_abs, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        bool changed = false;
        size_t len = strlen(current);

        for (size_t i = 1; i <= len; i++) {
            if (current[i] != '/' && current[i] != '\0') continue;

            char prefix[256];
            if (i >= sizeof(prefix)) break;
            strncpy(prefix, current, i);
            prefix[i] = '\0';

            char target[256];
            if (find_tar_symlink(prefix, target, sizeof(target))) {
                char next[256];
                strncpy(next, target, sizeof(next) - 1);
                next[sizeof(next) - 1] = '\0';

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

static void add_modified_file(const char *path, void *data, size_t size, uint32_t mode) {
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (!modified_files[i].is_active) {
            modified_files[i].is_active = true;
            strncpy(modified_files[i].path, path, sizeof(modified_files[i].path) - 1);
            modified_files[i].path[sizeof(modified_files[i].path) - 1] = '\0';
            modified_files[i].data = data;
            modified_files[i].size = size;
            modified_files[i].mode = mode;
            return;
        }
    }
}

rootfs_file_t read_rootfs(const char *path) {
    static int depth = 0;

    char abs_path[256];
    get_absolute_path(path, abs_path, sizeof(abs_path));

    char clean_path[256];
    collapse_slashes(abs_path, clean_path, sizeof(clean_path));

    char normalized_path[256];
    normalize_abs_components(clean_path, normalized_path, sizeof(normalized_path));

    char resolved_path[256];
    resolve_path_symlinks(normalized_path, resolved_path, sizeof(resolved_path));

    char norm[256];
    normalize_path(resolved_path, norm, sizeof(norm));

    rootfs_file_t result = { .data = NULL, .size = 0, .mode = 0 };

    if (depth >= 8) return result;
    depth++;

    // 1. Check overlay
    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            result.data = modified_files[i].data;
            result.size = modified_files[i].size;
            result.mode = modified_files[i].mode;
            depth--;
            return result;
        }
    }

    // 2. Check TAR archive (exact match + trailing slash tolerance)
    if (tar_archive_start == NULL) {
        depth--;
        return result;
    }

    uint8_t *ptr = tar_archive_start;
    while (1) {
        struct tar_header *h = (struct tar_header *)ptr;
        if (h->name[0] == '\0') break;

        uint64_t size = parse_octal(h->size);
        uint32_t mode = (uint32_t)parse_octal(h->mode);
        char type = h->typeflag[0];

        bool matches = tar_name_matches(h->name, norm);
        if (matches) {
            if (type == '1' || type == '2') {
                char target[101];
                strncpy(target, h->linkname, 100);
                target[100] = '\0';
                char resolved_abs[256];
                resolve_link_target(resolved_path, target, resolved_abs, sizeof(resolved_abs));
                depth--;
                return read_rootfs(resolved_abs);
            }
            if (type == '0' || type == '\0' || type == '5') {
                result.size = size;
                result.data = ptr + 512;
                result.mode = (type == '5') ? (mode | 0040000) : mode;
                depth--;
                return result;
            }
        }
        ptr += 512 + (size + 511) / 512 * 512; // Add padding
    }

    depth--;
    return result;
}

int write_rootfs(const char *path, const void *data, uint64_t size, uint32_t mode) {
    add_modified_file(path, (void *)data, (size_t)size, mode);
    return 0;
}

int delete_rootfs(const char *path) {
    char abs_path[256];
    get_absolute_path(path, abs_path, sizeof(abs_path));

    char norm[256];
    normalize_path(abs_path, norm, sizeof(norm));

    for (int i = 0; i < MAX_MODIFIED_FILES; i++) {
        if (modified_files[i].is_active && strcmp(modified_files[i].path, norm) == 0) {
            modified_files[i].is_active = false;
            return 0;
        }
    }
    return -1; // Not found
}

int mkdir_rootfs(const char *path, mode_t mode) {
    add_modified_file(path, NULL, 0, mode | 0x4000); // S_IFDIR
    return 0;
}

int get_rootfs_entry(int index, directory_entry_t *entry) {
    if (!tar_archive_start || !entry) return -1;

    uint8_t *ptr = tar_archive_start;
    int count = 0;
    int entry_index = 0;

    while (1) {
        struct tar_header *h = (struct tar_header *)ptr;
        if (h->name[0] == '\0') break;

        uint64_t size = parse_octal(h->size);
        char type = h->typeflag[0];

        if (count == index) {
            // Extract just the filename from the full path
            const char *name_part = strrchr(h->name, '/');
            if (name_part) name_part++;
            else name_part = h->name;

            // Strip trailing slash for directories
            size_t nlen = strlen(name_part);
            if (nlen > 1 && name_part[nlen - 1] == '/') {
                strncpy(entry->name, name_part, nlen - 1);
                entry->name[nlen - 1] = '\0';
            } else {
                strncpy(entry->name, name_part, sizeof(entry->name) - 1);
                entry->name[sizeof(entry->name) - 1] = '\0';
            }

            if (type == '5') entry->type = FT_DIRECTORY;
            else if (type == '0' || type == '\0') entry->type = FT_FILE;
            else entry->type = FT_FILE;

            return 0;
        }

        count++;
        ptr += 512 + (size + 511) / 512 * 512;
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
        if (!decompressed) {
            panic("Memory allocation failed.");
            return;
        }

        int result = ungzip(gz_data, decompressed);
        if (result < 0) {
            free(decompressed);
            return;
        }

        tar_archive_start = decompressed;
        tar_decompressed = decompressed;
    } else {
        panic("No module found.");
    }
    printf("Rootfs: Initialized rootfs.\n");
}
