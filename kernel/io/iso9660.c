#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mount.h>
#include <main/string.h>
#include <mm/mm.h>
#include <io/devices.h>
#include <io/iso9660.h>

iso9660_mount_t iso9660_mounts[ISO9660_MAX_MOUNTS];
spinlock_t iso9660_lock = SPINLOCK_INIT;

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const char *get_iso9660_device_name(const char *source) {
    if (!source) return NULL;
    while (*source == '/') source++;
    if (strncmp(source, "dev/", 4) == 0) source += 4;
    return source;
}

static int read_iso9660_checked(const iso9660_mount_t *mnt, void *buf, uint64_t count, uint64_t offset) {
    if (!mnt || !buf) return -EINVAL;
    if (offset > mnt->device_size || count > mnt->device_size - offset) return -EIO;
    uint64_t got = read_device(mnt->device, buf, count, offset);
    if ((int64_t)got < 0) return (int)(int64_t)got;
    return got == count ? 0 : -EIO;
}

static bool check_path_under(const char *path, const char *target, const char **relative) {
    size_t n = strlen(target);
    if (strncmp(path, target, n) != 0 || (path[n] != '\0' && path[n] != '/')) return false;
    const char *rel = path + n;
    while (*rel == '/') rel++;
    if (relative) *relative = rel;
    return true;
}

static iso9660_mount_t *find_iso9660_mount(const char *path, const char **relative) {
    iso9660_mount_t *best = NULL;
    const char *best_rel = NULL;
    size_t best_len = 0;
    for (int i = 0; i < ISO9660_MAX_MOUNTS; i++) {
        const char *rel;
        if (!iso9660_mounts[i].active || !check_path_under(path, iso9660_mounts[i].target, &rel)) continue;
        size_t len = strlen(iso9660_mounts[i].target);
        if (!best || len > best_len) {
            best = &iso9660_mounts[i];
            best_rel = rel;
            best_len = len;
        }
    }
    if (relative) *relative = best_rel;
    return best;
}

static int64_t build_iso9660_time(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second) {
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    int64_t shifted = month <= 2 ? month + 9 : month - 3;
    int64_t years = month <= 2 ? year - 1 : year;
    int64_t era = years / 400;
    int64_t year_of_era = years - era * 400;
    int64_t day_of_year = (153 * shifted + 2) / 5 + day - 1;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days = era * 146097 + day_of_era - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static int64_t convert_iso9660_time(const uint8_t raw_time[7]) {
    return build_iso9660_time(1900 + raw_time[0], raw_time[1], raw_time[2], raw_time[3], raw_time[4], raw_time[5]);
}

static int64_t convert_iso9660_long_time(const uint8_t raw_time[17]) {
    return build_iso9660_time(1900 + raw_time[0], raw_time[1], raw_time[2], raw_time[3], raw_time[4], raw_time[5]);
}

static void trim_iso9660_name(char *name, size_t *name_len) {
    size_t len = 0;
    while (len < *name_len && name[len] != ';') len++;
    while (len > 0 && name[len - 1] == '.') len--;
    name[len] = '\0';
    *name_len = len;
}

static void clean_iso9660_name(const uint8_t *raw_name, size_t raw_len, char *out_name, size_t *out_len) {
    size_t len = 0;
    while (len < raw_len && raw_name[len] != ';') len++;
    while (len > 0 && raw_name[len - 1] == '.') len--;
    memcpy(out_name, raw_name, len);
    out_name[len] = '\0';
    *out_len = len;
}

// Joliet names are UCS-2 big endian, convert to UTF-8.
static void convert_iso9660_ucs2_name(const uint8_t *raw_name, size_t raw_len, char *out_name, size_t *out_len) {
    size_t out = 0;
    for (size_t pos = 0; pos + 1 < raw_len && out + 3 < ISO9660_NAME_MAX - 1; pos += 2) {
        uint32_t code = ((uint32_t)raw_name[pos] << 8) | raw_name[pos + 1];
        if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
        if (code < 0x80) {
            out_name[out++] = (char)code;
        } else if (code < 0x800) {
            out_name[out++] = (char)(0xC0 | (code >> 6));
            out_name[out++] = (char)(0x80 | (code & 0x3F));
        } else {
            out_name[out++] = (char)(0xE0 | (code >> 12));
            out_name[out++] = (char)(0x80 | ((code >> 6) & 0x3F));
            out_name[out++] = (char)(0x80 | (code & 0x3F));
        }
    }
    out_name[out] = '\0';
    *out_len = out;
}

static bool use_iso9660_joliet(const iso9660_mount_t *mnt) {
    return !mnt->has_rock_ridge && mnt->has_joliet;
}

static void append_iso9660_name(iso9660_record_t *record, const uint8_t *text, size_t text_len) {
    if (record->name_len + 1 >= ISO9660_NAME_MAX) return;
    if (text_len > ISO9660_NAME_MAX - 1 - record->name_len) text_len = ISO9660_NAME_MAX - 1 - record->name_len;
    memcpy(record->name + record->name_len, text, text_len);
    record->name_len += text_len;
    record->name[record->name_len] = '\0';
}

static void append_iso9660_link_target(iso9660_record_t *record, const char *text, size_t text_len) {
    size_t built_len = strlen(record->link_target);
    if (built_len + 1 >= ISO9660_NAME_MAX) return;
    if (text_len > ISO9660_NAME_MAX - 1 - built_len) text_len = ISO9660_NAME_MAX - 1 - built_len;
    memcpy(record->link_target + built_len, text, text_len);
    record->link_target[built_len + text_len] = '\0';
}

static void apply_iso9660_rock_ridge_nm(const uint8_t *entry, uint8_t entry_len, iso9660_record_t *record) {
    if (entry_len < 5) return;
    if (entry[4] & (ISO9660_ROCK_RIDGE_NM_CURRENT | ISO9660_ROCK_RIDGE_NM_PARENT)) return;
    if (!record->rock_ridge_named) {
        record->name_len = 0;
        record->rock_ridge_named = true;
    }
    append_iso9660_name(record, entry + 5, entry_len - 5);
}

static void apply_iso9660_rock_ridge_px(const uint8_t *entry, uint8_t entry_len, iso9660_record_t *record) {
    if (entry_len < 36) return;
    record->rock_ridge_mode = read_le32(entry + 4);
    record->rock_ridge_nlink = read_le32(entry + 12);
    record->rock_ridge_uid = read_le32(entry + 20);
    record->rock_ridge_gid = read_le32(entry + 28);
    if (entry_len >= 44) record->rock_ridge_ino = read_le32(entry + 36);
}

static void apply_iso9660_rock_ridge_tf(const uint8_t *entry, uint8_t entry_len, iso9660_record_t *record) {
    if (entry_len < 5) return;
    uint8_t flags = entry[4];
    size_t step = (flags & ISO9660_ROCK_RIDGE_TF_LONG) ? 17 : 7;
    size_t pos = 5;
    bool found = false;
    int64_t picked = 0;
    for (uint16_t bit = 1; bit <= 0x40; bit <<= 1) {
        if (!(flags & bit)) continue;
        if (pos + step > entry_len) break;
        int64_t stamp = (step == 17) ? convert_iso9660_long_time(entry + pos) : convert_iso9660_time(entry + pos);
        if (!found) {
            picked = stamp;
            found = true;
        } else if (bit == ISO9660_ROCK_RIDGE_TF_MODIFY) picked = stamp;
        pos += step;
    }
    if (found) record->record_time = picked;
}

static void apply_iso9660_rock_ridge_sl(const uint8_t *entry, uint8_t entry_len, iso9660_record_t *record) {
    if (entry_len < 5) return;
    if (entry[4] & ISO9660_ROCK_RIDGE_SL_ROOT) append_iso9660_link_target(record, "/", 1);
    size_t pos = 5;
    while (pos + 2 <= entry_len) {
        uint8_t comp_flags = entry[pos];
        uint8_t comp_len = entry[pos + 1];
        pos += 2;
        if (pos + comp_len > entry_len) break;
        size_t built_len = strlen(record->link_target);
        if (built_len && record->link_target[built_len - 1] != '/') append_iso9660_link_target(record, "/", 1);
        if (comp_flags & ISO9660_ROCK_RIDGE_SL_CURRENT) append_iso9660_link_target(record, ".", 1);
        else if (comp_flags & ISO9660_ROCK_RIDGE_SL_PARENT) append_iso9660_link_target(record, "..", 2);
        else append_iso9660_link_target(record, (const char *)entry + pos, comp_len);
        pos += comp_len;
    }
    record->rock_ridge_symlink = true;
}

static void parse_iso9660_rock_ridge_sua(const iso9660_mount_t *mnt, const uint8_t *sua, size_t sua_len, int depth, iso9660_record_t *record);

static void load_iso9660_rock_ridge_ce(const iso9660_mount_t *mnt, const uint8_t *entry, uint8_t entry_len, int depth, iso9660_record_t *record) {
    if (entry_len < 28 || depth >= ISO9660_ROCK_RIDGE_CE_DEPTH_MAX) return;
    uint32_t area_lba = read_le32(entry + 4);
    uint32_t area_off = read_le32(entry + 12);
    uint32_t area_len = read_le32(entry + 20);
    if (!area_len || area_len > ISO9660_ROCK_RIDGE_CE_BYTES_MAX) return;
    uint64_t area_base = (uint64_t)area_lba * mnt->sector_size + area_off;
    if (area_base > mnt->device_size || area_len > mnt->device_size - area_base) return;
    uint8_t *area = malloc(area_len);
    if (!area) return;
    if (read_iso9660_checked(mnt, area, area_len, area_base) == 0) parse_iso9660_rock_ridge_sua(mnt, area, area_len, depth + 1, record);
    free(area);
}

static void parse_iso9660_rock_ridge_sua(const iso9660_mount_t *mnt, const uint8_t *sua, size_t sua_len, int depth, iso9660_record_t *record) {
    size_t pos = 0;
    while (pos + 4 <= sua_len) {
        uint8_t entry_len = sua[pos + 2];
        if (entry_len < 4 || pos + entry_len > sua_len) break;
        const uint8_t *entry = sua + pos;
        if (entry[0] == 'N' && entry[1] == 'M') apply_iso9660_rock_ridge_nm(entry, entry_len, record);
        else if (entry[0] == 'P' && entry[1] == 'X') apply_iso9660_rock_ridge_px(entry, entry_len, record);
        else if (entry[0] == 'T' && entry[1] == 'F') apply_iso9660_rock_ridge_tf(entry, entry_len, record);
        else if (entry[0] == 'S' && entry[1] == 'L') apply_iso9660_rock_ridge_sl(entry, entry_len, record);
        else if (entry[0] == 'C' && entry[1] == 'E') load_iso9660_rock_ridge_ce(mnt, entry, entry_len, depth, record);
        pos += entry_len;
    }
}

static int parse_iso9660_record(const iso9660_mount_t *mnt, const uint8_t *raw, size_t rec_len, iso9660_record_t *record) {
    if (rec_len < ISO9660_RECORD_NAME_OFFSET) return -EIO;
    uint8_t name_len = raw[ISO9660_RECORD_NAME_LENGTH_OFFSET];
    if ((size_t)ISO9660_RECORD_NAME_OFFSET + name_len > rec_len) return -EIO;
    uint32_t lba = read_le32(raw + ISO9660_RECORD_LBA_OFFSET);
    uint32_t lba_be = read_be32(raw + ISO9660_RECORD_LBA_OFFSET + 4);
    if (lba_be && lba_be != lba) return -EIO;
    uint32_t size = read_le32(raw + ISO9660_RECORD_SIZE_OFFSET);
    uint32_t size_be = read_be32(raw + ISO9660_RECORD_SIZE_OFFSET + 4);
    if (size_be && size_be != size) return -EIO;
    memset(record, 0, sizeof(*record));
    record->record_lba = lba;
    record->record_size = size;
    record->record_xar = raw[ISO9660_RECORD_XAR_OFFSET];
    record->record_flags = raw[ISO9660_RECORD_FLAGS_OFFSET];
    record->record_time = convert_iso9660_time(raw + ISO9660_RECORD_TIME_OFFSET);
    if (name_len == 1 && (raw[ISO9660_RECORD_NAME_OFFSET] == 0 || raw[ISO9660_RECORD_NAME_OFFSET] == 1)) return 0;
    if (use_iso9660_joliet(mnt)) convert_iso9660_ucs2_name(raw + ISO9660_RECORD_NAME_OFFSET, name_len, record->name, &record->name_len);
    else clean_iso9660_name(raw + ISO9660_RECORD_NAME_OFFSET, name_len, record->name, &record->name_len);
    if (use_iso9660_joliet(mnt)) trim_iso9660_name(record->name, &record->name_len);
    if (mnt->has_rock_ridge) {
        size_t sua_offset = ISO9660_RECORD_NAME_OFFSET + name_len;
        sua_offset += sua_offset & 1;
        if (sua_offset < rec_len) parse_iso9660_rock_ridge_sua(mnt, raw + sua_offset, rec_len - sua_offset, 0, record);
    }
    if (record->rock_ridge_symlink && !record->rock_ridge_mode) record->rock_ridge_mode = S_IFLNK | 0777;
    return 0;
}

static void setup_iso9660_root(const iso9660_mount_t *mnt, iso9660_record_t *record) {
    memset(record, 0, sizeof(*record));
    record->record_flags = ISO9660_FLAG_DIR;
    if (use_iso9660_joliet(mnt)) {
        record->record_lba = mnt->joliet_root_lba;
        record->record_size = mnt->joliet_root_size;
    } else {
        record->record_lba = mnt->root_lba;
        record->record_size = mnt->root_size;
    }
}

static int walk_iso9660_dir(const iso9660_mount_t *mnt, uint32_t dir_lba, uint32_t dir_size, iso9660_dir_callback_t callback, void *context) {
    if (!dir_size || dir_size > ISO9660_DIR_MAX_BYTES) return -EIO;
    if ((uint64_t)dir_lba * mnt->sector_size > mnt->device_size) return -EIO;
    uint8_t *dir = malloc(dir_size);
    if (!dir) return -ENOMEM;
    if (read_iso9660_checked(mnt, dir, dir_size, (uint64_t)dir_lba * mnt->sector_size) < 0) {
        free(dir);
        return -EIO;
    }
    int result = 0;
    size_t pos = 0;
    while (pos < dir_size) {
        size_t sector_left = mnt->sector_size - pos % mnt->sector_size;
        uint8_t rec_len = dir[pos];
        if (rec_len == 0) {
            pos += sector_left;
            continue;
        }
        iso9660_record_t record;
        result = rec_len > sector_left ? -EIO : parse_iso9660_record(mnt, dir + pos, rec_len, &record);
        if (result < 0) break;
        if (record.name_len != 0) {
            result = callback(&record, context);
            if (result != 0) break;
        }
        pos += rec_len;
    }
    free(dir);
    return result;
}

static char lower_iso9660_char(char c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool match_iso9660_names(const char *wanted, size_t wanted_len, const char *name) {
    if (strlen(name) != wanted_len) return false;
    for (size_t i = 0; i < wanted_len; i++) if (lower_iso9660_char(wanted[i]) != lower_iso9660_char(name[i])) return false;
    return true;
}

static int match_iso9660_child(const iso9660_record_t *record, void *context) {
    iso9660_lookup_context_t *ctx = context;
    if (!ctx->matched && match_iso9660_names(ctx->component, ctx->component_len, record->name)) {
        ctx->matched = true;
        ctx->found = *record;
        return 1;
    }
    return 0;
}

static void join_iso9660_path(char *out, size_t out_size, const char *dir, size_t dir_len, const char *leaf, size_t leaf_len) {
    size_t pos = dir_len < out_size - 1 ? dir_len : out_size - 1;
    memmove(out, dir, pos);
    if (pos && out[pos - 1] != '/' && pos < out_size - 1) out[pos++] = '/';
    for (size_t i = 0; i < leaf_len && pos < out_size - 1; i++) out[pos++] = leaf[i];
    out[pos] = '\0';
}

static int resolve_iso9660_record(const iso9660_mount_t *mnt, const char *relative, iso9660_record_t *resolved) {
    iso9660_record_t current;
    setup_iso9660_root(mnt, &current);
    char built[ISO9660_RELATIVE_MAX];
    size_t built_len = 0;
    built[0] = '\0';
    size_t pos = 0;
    while (relative[pos]) {
        while (relative[pos] == '/') pos++;
        if (!relative[pos]) break;
        size_t start = pos;
        while (relative[pos] && relative[pos] != '/') pos++;
        size_t len = pos - start;
        if (len > ISO9660_MAX_COMPONENT) return -ENAMETOOLONG;
        if (len == 1 && relative[start] == '.') continue;
        if (len == 2 && relative[start] == '.' && relative[start + 1] == '.') {
            while (built_len > 0 && built[built_len - 1] != '/') built_len--;
            if (built_len > 0) built_len--;
            built[built_len] = '\0';
            if (built_len == 0) setup_iso9660_root(mnt, &current);
            else {
                int up_status = resolve_iso9660_record(mnt, built, &current);
                if (up_status < 0) return up_status;
            }
            continue;
        }
        if (!(current.record_flags & ISO9660_FLAG_DIR)) return -ENOTDIR;
        iso9660_lookup_context_t ctx = {relative + start, len, false, {0}};
        int status = walk_iso9660_dir(mnt, current.record_lba, current.record_size, match_iso9660_child, &ctx);
        if (status < 0) return status;
        if (!ctx.matched) return -ENOENT;
        current = ctx.found;
        join_iso9660_path(built, sizeof(built), built, built_len, relative + start, len);
        built_len = strlen(built);
    }
    *resolved = current;
    return 0;
}

// Follow a final symlink chain, absolute targets are relative to the mount root.
static int follow_iso9660_record(const iso9660_mount_t *mnt, const char *relative, iso9660_record_t *resolved) {
    char current[ISO9660_RELATIVE_MAX];
    strlcpy(current, relative, sizeof(current));
    for (int depth = 0; depth < ISO9660_ROCK_RIDGE_LINK_DEPTH_MAX; depth++) {
        int status = resolve_iso9660_record(mnt, current, resolved);
        if (status < 0) return status;
        if (!resolved->rock_ridge_mode || !S_ISLNK(resolved->rock_ridge_mode)) return 0;
        char target[ISO9660_NAME_MAX];
        strlcpy(target, resolved->link_target, sizeof(target));
        if (target[0] == '/') {
            strlcpy(current, target + 1, sizeof(current));
            continue;
        }
        size_t parent_len = strlen(current);
        while (parent_len > 0 && current[parent_len - 1] != '/') parent_len--;
        char joined[ISO9660_RELATIVE_MAX];
        join_iso9660_path(joined, sizeof(joined), current, parent_len, target, strlen(target));
        strlcpy(current, joined, sizeof(current));
    }
    return -ELOOP;
}

static bool probe_iso9660_joliet(iso9660_mount_t *probe) {
    uint8_t vd[ISO9660_SECTOR_SIZE];
    for (uint32_t sector = ISO9660_PVD_SECTOR + 1; sector <= ISO9660_VD_SECTOR_LAST; sector++) {
        if (read_iso9660_checked(probe, vd, sizeof(vd), (uint64_t)sector * ISO9660_SECTOR_SIZE) < 0) return false;
        if (memcmp(vd + 1, "CD001", 5) != 0) return false;
        if (vd[0] == ISO9660_VD_TYPE_TERMINATOR) return false;
        if (vd[0] != ISO9660_VD_TYPE_SUPPLEMENTARY) continue;
        const uint8_t *escape = vd + ISO9660_SVD_ESCAPE_OFFSET;
        if (escape[0] != '%' || escape[1] != '/' || (escape[2] != '@' && escape[2] != 'C' && escape[2] != 'E')) continue;
        if (read_le16(vd + ISO9660_PVD_BLOCK_SIZE_OFFSET) != ISO9660_SECTOR_SIZE) continue;
        const uint8_t *root = vd + ISO9660_PVD_ROOT_OFFSET;
        if (root[0] < ISO9660_RECORD_NAME_OFFSET) continue;
        uint32_t root_lba = read_le32(root + ISO9660_RECORD_LBA_OFFSET);
        uint32_t root_size = read_le32(root + ISO9660_RECORD_SIZE_OFFSET);
        if (!root_size || root_size > ISO9660_DIR_MAX_BYTES) continue;
        if ((uint64_t)root_lba * ISO9660_SECTOR_SIZE > probe->device_size) continue;
        probe->joliet_root_lba = root_lba;
        probe->joliet_root_size = root_size;
        return true;
    }
    return false;
}

static bool probe_iso9660_rock_ridge(const iso9660_mount_t *probe) {
    if (!probe->root_size || probe->root_size > ISO9660_DIR_MAX_BYTES) return false;
    uint8_t sector[ISO9660_SECTOR_SIZE];
    if (read_iso9660_checked(probe, sector, probe->sector_size, (uint64_t)probe->root_lba * probe->sector_size) < 0) return false;
    uint8_t rec_len = sector[0];
    uint8_t name_len = sector[ISO9660_RECORD_NAME_LENGTH_OFFSET];
    if (rec_len < ISO9660_RECORD_NAME_OFFSET + name_len) return false;
    size_t sua_offset = ISO9660_RECORD_NAME_OFFSET + name_len;
    sua_offset += sua_offset & 1;
    if (rec_len < sua_offset + 2) return false;
    return sector[sua_offset] == 'S' && sector[sua_offset + 1] == 'P';
}

int mount_iso9660(const char *source, const char *path, unsigned long flags, const char *data) {
    if (!source || !source[0] || !path || path[0] != '/' || strlen(path) >= ISO9660_TARGET_MAX) return -EINVAL;
    if (flags & ~(MS_RDONLY | MS_SILENT)) return -EOPNOTSUPP;
    if (data && data[0] && strcmp(data, "ro") != 0) return -EOPNOTSUPP;
    const char *dev = get_iso9660_device_name(source);
    if (!dev || !*dev || strlen(dev) >= ISO9660_DEVICE_MAX) return -EINVAL;
    uint64_t device_size;
    int status = get_block_device_size(dev, &device_size);
    if (status < 0) return status;
    if (device_size < (uint64_t)(ISO9660_PVD_SECTOR + 1) * ISO9660_SECTOR_SIZE) return -EINVAL;

    iso9660_mount_t probe;
    memset(&probe, 0, sizeof(probe));
    strlcpy(probe.device, dev, sizeof(probe.device));
    probe.device_size = device_size;
    probe.sector_size = ISO9660_SECTOR_SIZE;

    uint8_t vd[ISO9660_SECTOR_SIZE];
    status = read_iso9660_checked(&probe, vd, sizeof(vd), (uint64_t)ISO9660_PVD_SECTOR * ISO9660_SECTOR_SIZE);
    if (status < 0) return status;
    if (vd[0] != ISO9660_VD_TYPE_PRIMARY || memcmp(vd + 1, "CD001", 5) != 0 || vd[6] != 1) return -EINVAL;
    if (read_le16(vd + ISO9660_PVD_BLOCK_SIZE_OFFSET) != ISO9660_SECTOR_SIZE) return -EOPNOTSUPP;
    const uint8_t *root = vd + ISO9660_PVD_ROOT_OFFSET;
    if (root[0] < ISO9660_RECORD_NAME_OFFSET) return -EINVAL;
    probe.root_lba = read_le32(root + ISO9660_RECORD_LBA_OFFSET);
    probe.root_size = read_le32(root + ISO9660_RECORD_SIZE_OFFSET);
    if (!probe.root_size || probe.root_size > ISO9660_DIR_MAX_BYTES) return -EINVAL;
    if ((uint64_t)probe.root_lba * probe.sector_size > device_size) return -EINVAL;

    probe.has_joliet = probe_iso9660_joliet(&probe);
    probe.has_rock_ridge = probe_iso9660_rock_ridge(&probe);

    strlcpy(probe.target, path, sizeof(probe.target));
    size_t target_len = strlen(probe.target);
    while (target_len > 1 && probe.target[target_len - 1] == '/') probe.target[--target_len] = '\0';

    uint64_t irq;
    spin_lock_irqsave(&iso9660_lock, &irq);
    int slot = -1;
    for (int i = 0; i < ISO9660_MAX_MOUNTS; i++) {
        if (iso9660_mounts[i].active && strcmp(iso9660_mounts[i].target, probe.target) == 0) {
            spin_unlock_irqrestore(&iso9660_lock, irq);
            return -EBUSY;
        }
        if (!iso9660_mounts[i].active && slot < 0) slot = i;
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&iso9660_lock, irq);
        return -ENOSPC;
    }
    iso9660_mounts[slot] = probe;
    iso9660_mounts[slot].active = true;
    spin_unlock_irqrestore(&iso9660_lock, irq);
    return 0;
}

int unmount_iso9660(const char *path) {
    if (!path) return -EINVAL;
    char normalized[ISO9660_TARGET_MAX];
    if (strlen(path) >= sizeof(normalized)) return -ENAMETOOLONG;
    strlcpy(normalized, path, sizeof(normalized));
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/') normalized[--length] = '\0';
    uint64_t irq;
    spin_lock_irqsave(&iso9660_lock, &irq);
    for (int i = 0; i < ISO9660_MAX_MOUNTS; i++) {
        if (iso9660_mounts[i].active && strcmp(iso9660_mounts[i].target, normalized) == 0) {
            memset(&iso9660_mounts[i], 0, sizeof(iso9660_mounts[i]));
            spin_unlock_irqrestore(&iso9660_lock, irq);
            return 0;
        }
    }
    spin_unlock_irqrestore(&iso9660_lock, irq);
    return -ENOENT;
}

bool check_iso9660_path(const char *path) {
    return path && find_iso9660_mount(path, NULL) != NULL;
}

bool get_iso9660_mount_root(const char *path, char *root, size_t root_size) {
    if (!path || !root || !root_size) return false;
    iso9660_mount_t *mnt = find_iso9660_mount(path, NULL);
    if (!mnt) return false;
    strlcpy(root, mnt->target, root_size);
    return true;
}

int stat_iso9660(const char *path, struct stat *st, bool follow) {
    if (!path || !st) return -EINVAL;
    const char *relative;
    iso9660_mount_t *mnt = find_iso9660_mount(path, &relative);
    if (!mnt) return -ENOENT;
    iso9660_record_t record;
    int status = follow ? follow_iso9660_record(mnt, relative, &record) : resolve_iso9660_record(mnt, relative, &record);
    if (status < 0) return status;
    memset(st, 0, sizeof(*st));
    uint32_t file_mode = record.rock_ridge_mode;
    if (!file_mode) file_mode = (record.record_flags & ISO9660_FLAG_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0555);
    if ((file_mode & S_IFMT) == 0) file_mode |= (record.record_flags & ISO9660_FLAG_DIR) ? S_IFDIR : S_IFREG;
    st->st_ino = record.rock_ridge_ino ? record.rock_ridge_ino : record.record_lba;
    st->st_mode = file_mode;
    st->st_nlink = record.rock_ridge_nlink ? record.rock_ridge_nlink : ((record.record_flags & ISO9660_FLAG_DIR) ? 2 : 1);
    st->st_uid = record.rock_ridge_uid;
    st->st_gid = record.rock_ridge_gid;
    st->st_size = S_ISLNK(file_mode) ? strlen(record.link_target) : record.record_size;
    st->st_blksize = mnt->sector_size;
    st->st_blocks = ((uint64_t)record.record_size + 511) / 512;
    st->st_atime = record.record_time;
    st->st_mtime = record.record_time;
    st->st_ctime = record.record_time;
    return 0;
}

int read_iso9660_link(const char *path, char *target, size_t target_size) {
    if (!path || !target || !target_size) return -EINVAL;
    const char *relative;
    iso9660_mount_t *mnt = find_iso9660_mount(path, &relative);
    if (!mnt) return -ENOENT;
    iso9660_record_t record;
    int status = resolve_iso9660_record(mnt, relative, &record);
    if (status < 0) return status;
    if (!record.rock_ridge_mode || !S_ISLNK(record.rock_ridge_mode)) return -EINVAL;
    size_t len = strlen(record.link_target);
    if (len >= target_size) len = target_size - 1;
    memcpy(target, record.link_target, len);
    target[len] = '\0';
    return (int)len;
}

int64_t read_iso9660(const char *path, void *buffer, uint64_t count, uint64_t offset) {
    if (!path || (!buffer && count)) return -EINVAL;
    const char *relative;
    iso9660_mount_t *mnt = find_iso9660_mount(path, &relative);
    if (!mnt) return -ENOENT;
    iso9660_record_t record;
    int status = follow_iso9660_record(mnt, relative, &record);
    if (status < 0) return status;
    if (record.record_flags & ISO9660_FLAG_DIR) return -EISDIR;
    if (record.record_size > mnt->device_size) return -EIO;
    if (offset >= record.record_size) return 0;
    uint64_t bytes = count < (uint64_t)record.record_size - offset ? count : (uint64_t)record.record_size - offset;
    uint64_t data_lba = (uint64_t)(record.record_lba + record.record_xar);
    if (data_lba > UINT64_MAX / mnt->sector_size) return -EOVERFLOW;
    status = read_iso9660_checked(mnt, buffer, bytes, data_lba * mnt->sector_size + offset);
    if (status < 0) return status;
    return (int64_t)bytes;
}

static uint8_t get_iso9660_dirent_type(const iso9660_record_t *record) {
    if (!record->rock_ridge_mode) return (record->record_flags & ISO9660_FLAG_DIR) ? DT_DIR : DT_REG;
    if (S_ISDIR(record->rock_ridge_mode)) return DT_DIR;
    if (S_ISLNK(record->rock_ridge_mode)) return DT_LNK;
    if (S_ISCHR(record->rock_ridge_mode)) return DT_CHR;
    if (S_ISBLK(record->rock_ridge_mode)) return DT_BLK;
    if (S_ISFIFO(record->rock_ridge_mode)) return DT_FIFO;
    if (S_ISSOCK(record->rock_ridge_mode)) return DT_SOCK;
    return DT_REG;
}

static int select_iso9660_child(const iso9660_record_t *record, void *context) {
    iso9660_readdir_context_t *ctx = context;
    if (ctx->seen++ != ctx->wanted) return 0;
    if (ctx->name_size < record->name_len + 1) return -ENAMETOOLONG;
    memcpy(ctx->name, record->name, record->name_len);
    ctx->name[record->name_len] = '\0';
    *ctx->type = get_iso9660_dirent_type(record);
    *ctx->ino = record->rock_ridge_ino ? record->rock_ridge_ino : record->record_lba;
    return 1;
}

int get_next_iso9660_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino) {
    if (!index || *index < 0 || !path || !name || !name_size || !type || !ino) return -EINVAL;
    const char *relative;
    iso9660_mount_t *mnt = find_iso9660_mount(path, &relative);
    if (!mnt) return -ENOENT;
    iso9660_record_t dir;
    int status = follow_iso9660_record(mnt, relative, &dir);
    if (status < 0) return status;
    if (!(dir.record_flags & ISO9660_FLAG_DIR)) return -ENOTDIR;
    iso9660_readdir_context_t ctx = {*index, 0, name, name_size, type, ino};
    status = walk_iso9660_dir(mnt, dir.record_lba, dir.record_size, select_iso9660_child, &ctx);
    if (status == 1) {
        (*index)++;
        return 0;
    }
    return status < 0 ? status : -ENOENT;
}
