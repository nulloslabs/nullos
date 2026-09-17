#include <stdbool.h>
#include <errno.h>
#include <main/string.h>
#include <io/devtmpfs.h>
#include <io/vfs.h>

bool device_exists_on_devtmpfs(const char* name) {
    while (*name == '/') name++;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, name) == 0) {
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return true;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return false;
}

bool is_devtmpfs_path(const char *path, char *rel_out) {
    return match_vfs_path(path, "devtmpfs", rel_out);
}

const char *get_devtmpfs_device_name(int index) {
    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    int count = 0;
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active) continue;
        if (count == index) {
            const char *name = devtmpfs_devices[i].name;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return name;
        }
        count++;
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return NULL;
}

// True when name sits directly under dir (dir "" matches all top level).
// Rest points past "dir/" (or at name when dir is empty).
static bool match_devtmpfs_dir(const char *name, const char *dir, const char **rest_out) {
    if (!name || !dir) return false;
    if (dir[0] == '\0') {
        if (rest_out) *rest_out = name;
        return true;
    }
    size_t len = strlen(dir);
    if (strncmp(name, dir, len) != 0 || name[len] != '/') return false;
    if (rest_out) *rest_out = name + len + 1;
    return true;
}

bool devtmpfs_is_dir(const char *rel) {
    if (!rel || rel[0] == '\0') return false;
    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active) continue;
        const char *rest = 0;
        if (!match_devtmpfs_dir(devtmpfs_devices[i].name, rel, &rest) || rest[0] == '\0') continue;
        spin_unlock_irqrestore(&devtmpfs_lock, irq);
        return true;
    }
    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return false;
}

int get_devtmpfs_dirent(const char *dir, int index, char *name_out, size_t name_size, bool *is_dir_out) {
    if (!dir || index < 0 || !name_out || name_size == 0) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);
    int seq = 0;
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active) continue;
        const char *rest = 0;
        if (!match_devtmpfs_dir(devtmpfs_devices[i].name, dir, &rest) || rest[0] == '\0') continue;
        size_t comp_len = 0;
        while (rest[comp_len] != '\0' && rest[comp_len] != '/') comp_len++;
        bool dupe = false;
        for (int j = 0; j < i; j++) {
            if (!devtmpfs_devices[j].active) continue;
            const char *prev = 0;
            if (!match_devtmpfs_dir(devtmpfs_devices[j].name, dir, &prev) || prev[0] == '\0') continue;
            size_t k = 0;
            while (k < comp_len && prev[k] == rest[k]) k++;
            if (k == comp_len && (prev[k] == '\0' || prev[k] == '/')) { dupe = true; break; }
        }
        if (dupe) continue;
        if (seq == index) {
            size_t copy = comp_len < name_size - 1 ? comp_len : name_size - 1;
            memcpy(name_out, rest, copy);
            name_out[copy] = '\0';
            if (is_dir_out) *is_dir_out = rest[comp_len] == '/';
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return 0;
        }
        seq++;
    }
    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOENT;
}

bool is_devtmpfs_dir_path(const char *path) {
    if (!path) return false;
    char rel[256];
    if (!is_devtmpfs_path(path, rel) || rel[0] == '\0') return false;
    return devtmpfs_is_dir(rel);
}
