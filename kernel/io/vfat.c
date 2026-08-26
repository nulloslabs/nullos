#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <main/string.h>
#include <main/timekeeping.h>
#include <io/devices.h>
#include <io/vfat.h>
#include <mm/mm.h>

vfat_mount_t vfat_mounts[VFAT_MAX_MOUNTS];
spinlock_t vfat_lock = SPINLOCK_INIT;

const uint8_t vfat_gpt_guid[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t read_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void write_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void write_le32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

static const char *get_device_name(const char *source) {
    if (!source) return NULL;
    while (*source == '/') source++;
    if (strncmp(source, "dev/", 4) == 0) source += 4;
    return source;
}

static bool check_path_under(const char *path, const char *target, const char **relative) {
    size_t n = strlen(target);
    if (strncmp(path, target, n) != 0) return false;
    if (path[n] != '\0' && path[n] != '/') return false;
    const char *rel = path + n;
    while (*rel == '/') rel++;
    if (relative) *relative = rel;
    return true;
}

static vfat_mount_t *find_mount(const char *path, const char **relative) {
    vfat_mount_t *best = NULL;
    const char *best_rel = NULL;
    size_t best_len = 0;
    for (int i = 0; i < VFAT_MAX_MOUNTS; i++) {
        const char *rel;
        if (!vfat_mounts[i].active || !check_path_under(path, vfat_mounts[i].target, &rel)) continue;
        size_t len = strlen(vfat_mounts[i].target);
        if (!best || len > best_len) { best = &vfat_mounts[i]; best_rel = rel; best_len = len; }
    }
    if (relative) *relative = best_rel;
    return best;
}

static int read_checked(const vfat_mount_t *mnt, void *buf, uint64_t count, uint64_t offset) {
    if (!mnt || !buf) return -EINVAL;
    uint64_t abs = mnt->partition_offset + offset;
    if (abs + count > mnt->device_size || abs < mnt->partition_offset) return -EIO;
    uint64_t got = read_device(mnt->device, buf, count, abs);
    if ((int64_t)got < 0) return (int)(int64_t)got;
    return got == count ? 0 : -EIO;
}

static int write_checked(const vfat_mount_t *mnt, const void *buf, uint64_t count, uint64_t offset) {
    if (!mnt || !buf) return -EINVAL;
    uint64_t abs = mnt->partition_offset + offset;
    if (abs + count > mnt->device_size || abs < mnt->partition_offset) return -EIO;
    uint64_t got = write_device(mnt->device, buf, count, abs);
    if ((int64_t)got < 0) return (int)(int64_t)got;
    return got == count ? 0 : -EIO;
}

static uint32_t get_next_cluster(const vfat_mount_t *mnt, uint32_t cluster) {
    if (mnt->fat_bits == 16) {
        if (cluster < 2 || cluster >= mnt->total_clusters + 2) return FAT32_CHAIN_EOF;
        uint64_t fat_byte = mnt->fat_offset + (uint64_t)cluster * 2;
        if (mnt->partition_offset + fat_byte + 2 > mnt->device_size) return FAT32_CHAIN_EOF;
        uint8_t entry[2];
        if (read_checked(mnt, entry, 2, fat_byte) != 0) return FAT32_CHAIN_EOF;
        uint32_t val = read_le16(entry);
        if (val >= 0xFFF8) return FAT32_CHAIN_EOF;
        return val;
    }
    if (cluster < 2 || cluster >= mnt->total_clusters + 2) return FAT32_CHAIN_EOF;
    uint64_t fat_byte = mnt->fat_offset + (uint64_t)cluster * 4;
    if (mnt->partition_offset + fat_byte + 4 > mnt->device_size) return FAT32_CHAIN_EOF;
    uint8_t entry[4];
    if (read_checked(mnt, entry, 4, fat_byte) != 0) return FAT32_CHAIN_EOF;
    uint32_t val = read_le32(entry) & 0x0FFFFFFF;
    if (val >= 0x0FFFFFF8) return FAT32_CHAIN_EOF;
    return val;
}

static uint32_t get_raw_cluster(const vfat_mount_t *mnt, uint32_t cluster) {
    if (mnt->fat_bits == 16) {
        if (cluster < 2 || cluster >= mnt->total_clusters + 2) return FAT32_CHAIN_EOF;
        uint64_t fat_byte = mnt->fat_offset + (uint64_t)cluster * 2;
        uint8_t entry[2];
        if (read_checked(mnt, entry, 2, fat_byte) != 0) return FAT32_CHAIN_EOF;
        return read_le16(entry);
    }
    if (cluster < 2 || cluster >= mnt->total_clusters + 2) return FAT32_CHAIN_EOF;
    uint64_t fat_byte = mnt->fat_offset + (uint64_t)cluster * 4;
    uint8_t entry[4];
    if (read_checked(mnt, entry, 4, fat_byte) != 0) return FAT32_CHAIN_EOF;
    return read_le32(entry) & 0x0FFFFFFF;
}

static int set_next_cluster(const vfat_mount_t *mnt, uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster >= mnt->total_clusters + 2) return -EIO;
    if (mnt->fat_bits == 16) {
        uint8_t entry[2];
        write_le16(entry, (uint16_t)value);
        for (uint32_t i = 0; i < mnt->num_fats; i++) {
            uint64_t fat_byte = mnt->fat_offset + (uint64_t)i * mnt->fat_size * mnt->bytes_per_sector + (uint64_t)cluster * 2;
            int r = write_checked(mnt, entry, 2, fat_byte);
            if (r < 0) return r;
        }
        return 0;
    }
    uint32_t masked = value & 0x0FFFFFFF;
    if (value >= FAT32_CHAIN_EOC && value <= FAT32_CHAIN_EOF) masked = value;
    for (uint32_t i = 0; i < mnt->num_fats; i++) {
        uint64_t fat_byte = mnt->fat_offset + (uint64_t)i * mnt->fat_size * mnt->bytes_per_sector + (uint64_t)cluster * 4;
        uint8_t cur[4];
        int r = read_checked(mnt, cur, 4, fat_byte);
        if (r < 0) return r;
        uint32_t old = read_le32(cur);
        uint32_t out = (old & 0xF0000000) | masked;
        uint8_t entry[4];
        write_le32(entry, out);
        r = write_checked(mnt, entry, 4, fat_byte);
        if (r < 0) return r;
    }
    return 0;
}

static int find_free_cluster(const vfat_mount_t *mnt, uint32_t *out) {
    for (uint32_t c = 2; c < mnt->total_clusters + 2; c++) {
        uint32_t v = get_raw_cluster(mnt, c);
        if (v == FAT32_CLUSTER_FREE) { *out = c; return 0; }
        if (v == FAT32_CHAIN_EOF && false) {}
    }
    return -ENOSPC;
}

static int count_chain(const vfat_mount_t *mnt, uint32_t first, uint32_t *out_count) {
    if (first < 2) { *out_count = 0; return 0; }
    uint32_t cnt = 0;
    uint32_t cur = first;
    while (cur >= 2 && cur < FAT32_CHAIN_EOF) {
        cnt++;
        if (cnt > mnt->total_clusters + 1) return -EIO;
        uint32_t nxt = get_next_cluster(mnt, cur);
        if (nxt >= FAT32_CHAIN_EOF) break;
        cur = nxt;
    }
    *out_count = cnt;
    return 0;
}

static int get_cluster_at_index(const vfat_mount_t *mnt, uint32_t first, uint32_t idx, uint32_t *out) {
    uint32_t cur = first;
    for (uint32_t i = 0; i < idx; i++) {
        if (cur < 2 || cur >= FAT32_CHAIN_EOF) return -EIO;
        cur = get_next_cluster(mnt, cur);
    }
    if (cur < 2 || cur >= FAT32_CHAIN_EOF) return -EIO;
    *out = cur;
    return 0;
}

static int get_last_cluster(const vfat_mount_t *mnt, uint32_t first, uint32_t *out_last) {
    if (first < 2) return -ENOENT;
    uint32_t cur = first;
    while (1) {
        uint32_t nxt = get_next_cluster(mnt, cur);
        if (nxt >= FAT32_CHAIN_EOF) { *out_last = cur; return 0; }
        cur = nxt;
    }
}

static int alloc_chain(const vfat_mount_t *mnt, uint32_t count, uint32_t *out_first) {
    if (count == 0) { *out_first = 0; return 0; }
    uint32_t first = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c;
        int r = find_free_cluster(mnt, &c);
        if (r < 0) {
            if (first) {
                uint32_t cur = first;
                while (cur >= 2 && cur < FAT32_CHAIN_EOF) {
                    uint32_t nxt = get_next_cluster(mnt, cur);
                    set_next_cluster(mnt, cur, FAT32_CLUSTER_FREE);
                    if (nxt >= FAT32_CHAIN_EOF) break;
                    cur = nxt;
                }
            }
            return r;
        }
        set_next_cluster(mnt, c, FAT32_CHAIN_EOF);
        uint8_t *zero = malloc(mnt->cluster_size);
        if (zero) {
            memset(zero, 0, mnt->cluster_size);
            uint64_t cluster_sector = mnt->data_offset / mnt->bytes_per_sector + (uint64_t)(c - 2) * mnt->sectors_per_cluster;
            uint64_t byte_off = cluster_sector * mnt->bytes_per_sector;
            write_checked(mnt, zero, mnt->cluster_size, byte_off);
            free(zero);
        }
        if (prev) set_next_cluster(mnt, prev, c);
        else first = c;
        prev = c;
    }
    *out_first = first;
    return 0;
}

static int free_chain(const vfat_mount_t *mnt, uint32_t first) {
    if (first < 2) return 0;
    uint32_t cur = first;
    while (cur >= 2 && cur < FAT32_CHAIN_EOF) {
        uint32_t nxt = get_raw_cluster(mnt, cur);
        if (nxt >= 0x0FFFFFF8 && mnt->fat_bits == 32) nxt = FAT32_CHAIN_EOF;
        else if (nxt >= 0xFFF8 && mnt->fat_bits == 16) nxt = FAT32_CHAIN_EOF;
        set_next_cluster(mnt, cur, FAT32_CLUSTER_FREE);
        if (nxt >= FAT32_CHAIN_EOF || nxt < 2) break;
        if (nxt >= mnt->total_clusters + 2) break;
        cur = nxt;
    }
    return 0;
}

static int extend_chain(const vfat_mount_t *mnt, uint32_t first, uint32_t needed_clusters, uint32_t *out_first) {
    if (needed_clusters == 0) { *out_first = first; return 0; }
    if (first < 2) return alloc_chain(mnt, needed_clusters, out_first);
    uint32_t have = 0;
    count_chain(mnt, first, &have);
    if (have >= needed_clusters) { *out_first = first; return 0; }
    uint32_t need = needed_clusters - have;
    uint32_t add_first = 0;
    int r = alloc_chain(mnt, need, &add_first);
    if (r < 0) return r;
    uint32_t last = 0;
    get_last_cluster(mnt, first, &last);
    set_next_cluster(mnt, last, add_first);
    *out_first = first;
    return 0;
}

static int truncate_chain(const vfat_mount_t *mnt, uint32_t first, uint32_t needed_clusters, uint32_t *out_first) {
    if (needed_clusters == 0) {
        if (first >= 2) free_chain(mnt, first);
        *out_first = 0;
        return 0;
    }
    if (first < 2) return alloc_chain(mnt, needed_clusters, out_first);
    uint32_t have = 0;
    count_chain(mnt, first, &have);
    if (have == needed_clusters) { *out_first = first; return 0; }
    if (have < needed_clusters) return extend_chain(mnt, first, needed_clusters, out_first);
    uint32_t keep_last = 0;
    get_cluster_at_index(mnt, first, needed_clusters - 1, &keep_last);
    uint32_t to_free = get_next_cluster(mnt, keep_last);
    set_next_cluster(mnt, keep_last, FAT32_CHAIN_EOF);
    if (to_free >= 2 && to_free < FAT32_CHAIN_EOF) free_chain(mnt, to_free);
    *out_first = first;
    return 0;
}

static int64_t read_cluster_data(const vfat_mount_t *mnt, uint32_t cluster, void *buf, uint64_t count, uint64_t offset) {
    if (cluster < 2 || cluster >= mnt->total_clusters + 2) return -EIO;
    if (offset >= mnt->cluster_size) return 0;
    if (count > mnt->cluster_size - offset) count = mnt->cluster_size - offset;
    uint64_t cluster_sector = mnt->data_offset / mnt->bytes_per_sector + (uint64_t)(cluster - 2) * mnt->sectors_per_cluster;
    uint64_t byte_off = cluster_sector * mnt->bytes_per_sector + offset;
    return read_checked(mnt, buf, count, byte_off) == 0 ? (int64_t)count : -EIO;
}

static int write_cluster_data(const vfat_mount_t *mnt, uint32_t cluster, const void *buf, uint64_t count, uint64_t offset) {
    if (cluster < 2 || cluster >= mnt->total_clusters + 2) return -EIO;
    if (offset >= mnt->cluster_size) return 0;
    if (count > mnt->cluster_size - offset) count = mnt->cluster_size - offset;
    uint64_t cluster_sector = mnt->data_offset / mnt->bytes_per_sector + (uint64_t)(cluster - 2) * mnt->sectors_per_cluster;
    uint64_t byte_off = cluster_sector * mnt->bytes_per_sector + offset;
    return write_checked(mnt, buf, count, byte_off) == 0 ? (int)count : -EIO;
}

static int64_t read_chain(const vfat_mount_t *mnt, uint32_t first_cluster, void *buf, uint64_t count, uint64_t offset, uint32_t file_size) {
    if (!buf && count) return -EINVAL;
    if (first_cluster < 2) return -EIO;
    uint8_t *out = buf;
    uint64_t done = 0;
    uint32_t cluster = first_cluster;
    while (done < count && offset + done < file_size) {
        uint64_t in_cluster = (offset + done) % mnt->cluster_size;
        uint64_t chunk = mnt->cluster_size - in_cluster;
        if (chunk > count - done) chunk = count - done;
        if (offset + done + chunk > file_size) chunk = file_size - offset - done;
        uint8_t local[512];
        void *dst = out ? out + done : local;
        int64_t got = read_cluster_data(mnt, cluster, dst, chunk, in_cluster);
        if (got < 0) return done ? (int64_t)done : got;
        done += (uint64_t)got;
        cluster = get_next_cluster(mnt, cluster);
        if (cluster >= FAT32_CHAIN_EOF) break;
    }
    return (int64_t)done;
}

static int64_t write_chain(const vfat_mount_t *mnt, uint32_t first_cluster, const void *buf, uint64_t count, uint64_t offset) {
    if (!buf && count) return -EINVAL;
    if (first_cluster < 2) return -EIO;
    const uint8_t *in = buf;
    uint64_t done = 0;
    uint32_t cluster = first_cluster;
    uint32_t idx = (uint32_t)(offset / mnt->cluster_size);
    int r = get_cluster_at_index(mnt, first_cluster, idx, &cluster);
    if (r < 0) return r;
    uint64_t cluster_off = offset % mnt->cluster_size;
    while (done < count) {
        uint64_t chunk = mnt->cluster_size - cluster_off;
        if (chunk > count - done) chunk = count - done;
        int64_t w = write_cluster_data(mnt, cluster, in + done, chunk, cluster_off);
        if (w < 0) return done ? (int64_t)done : w;
        done += (uint64_t)w;
        cluster_off = 0;
        if (done >= count) break;
        uint32_t nxt = get_next_cluster(mnt, cluster);
        if (nxt >= FAT32_CHAIN_EOF) return -EIO;
        cluster = nxt;
    }
    return (int64_t)done;
}

static void decode_short_name(const uint8_t *entry, char *out, size_t *out_len) {
    char name[9];
    char ext[4];
    size_t n = 0, e = 0;
    for (int i = 0; i < 8 && entry[i] != ' '; i++) name[n++] = (char)(entry[i] >= 'A' && entry[i] <= 'Z' ? entry[i] + 32 : entry[i]);
    for (int i = 8; i < 11 && entry[i] != ' '; i++) ext[e++] = (char)(entry[i] >= 'A' && entry[i] <= 'Z' ? entry[i] + 32 : entry[i]);
    name[n] = '\0'; ext[e] = '\0';
    if (e > 0) { memcpy(out, name, n); out[n] = '.'; memcpy(out + n + 1, ext, e); out[n + 1 + e] = '\0'; *out_len = n + 1 + e; }
    else { memcpy(out, name, n); out[n] = '\0'; *out_len = n; }
}

static void push_lfn_char(char *name, size_t *len, uint32_t code) {
    if (*len + 4 >= VFAT_MAX_NAME) return;
    if (code < 0x80) { name[(*len)++] = (char)code; }
    else if (code < 0x800) { name[(*len)++] = (char)(0xC0 | (code >> 6)); name[(*len)++] = (char)(0x80 | (code & 0x3F)); }
    else { name[(*len)++] = (char)(0xE0 | (code >> 12)); name[(*len)++] = (char)(0x80 | ((code >> 6) & 0x3F)); name[(*len)++] = (char)(0x80 | (code & 0x3F)); }
}

static void decode_lfn_name(const uint8_t *entry, char *name, size_t *len, uint8_t order) {
    if (order & 0x40) *len = 0;
    const uint16_t *name1 = (const uint16_t *)(entry + FAT32_LFN_NAME1);
    const uint16_t *name2 = (const uint16_t *)(entry + FAT32_LFN_NAME2);
    const uint16_t *name3 = (const uint16_t *)(entry + FAT32_LFN_NAME3);
    for (int i = 0; i < 5; i++) { uint16_t c = read_le16((const uint8_t *)&name1[i]); if (!c || c == 0xFFFF) return; push_lfn_char(name, len, c); }
    for (int i = 0; i < 6; i++) { uint16_t c = read_le16((const uint8_t *)&name2[i]); if (!c || c == 0xFFFF) return; push_lfn_char(name, len, c); }
    for (int i = 0; i < 2; i++) { uint16_t c = read_le16((const uint8_t *)&name3[i]); if (!c || c == 0xFFFF) return; push_lfn_char(name, len, c); }
}

static int walk_entries_buf(uint8_t *buf, uint32_t total_bytes, vfat_dir_callback_t callback, void *context) {
    uint8_t lfn_name[VFAT_MAX_NAME * 3];
    size_t lfn_len = 0;
    uint8_t lfn_checksum = 0;
    bool have_lfn = false;
    uint32_t entries = total_bytes / FAT32_DE_SIZE;
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t *entry = buf + i * FAT32_DE_SIZE;
        if (entry[0] == 0x00) return 0;
        if (entry[0] == 0xE5) { if (have_lfn) { lfn_len = 0; have_lfn = false; } continue; }
        if (entry[FAT32_DE_ATTR] == FAT32_ATTR_LFN) {
            uint8_t order = entry[FAT32_LFN_ORDER];
            uint8_t cs = entry[FAT32_LFN_CHECKSUM];
            if (order & 0x40) { lfn_len = 0; lfn_checksum = cs; }
            else if (cs != lfn_checksum) { lfn_len = 0; continue; }
            decode_lfn_name(entry, (char *)lfn_name, &lfn_len, order);
            if (!(order & 0x40) && order == 1) { lfn_name[lfn_len] = '\0'; have_lfn = true; }
            continue;
        }
        if (entry[FAT32_DE_ATTR] == FAT32_ATTR_VOLUME_ID) { lfn_len = 0; have_lfn = false; continue; }
        vfat_dirent_t dirent;
        memset(&dirent, 0, sizeof(dirent));
        dirent.first_cluster = (uint32_t)read_le16(entry + FAT32_DE_FSTCLUS_HI) << 16 | read_le16(entry + FAT32_DE_FSTCLUS_LO);
        dirent.file_size = read_le32(entry + FAT32_DE_FILESIZE);
        dirent.attr = entry[FAT32_DE_ATTR];
        if (have_lfn) { memcpy(dirent.long_name, lfn_name, lfn_len + 1); dirent.long_name_len = lfn_len; }
        decode_short_name(entry, dirent.short_name, &dirent.short_name_len);
        lfn_len = 0; have_lfn = false;
        int result = callback(&dirent, context);
        if (result != 0) return result;
    }
    return 0;
}

static int walk_directory(const vfat_mount_t *mnt, uint32_t dir_cluster, vfat_dir_callback_t callback, void *context) {
    if (mnt->fat_bits == 16 && dir_cluster == 0) {
        uint32_t bytes = (uint32_t)mnt->root_dir_entries * FAT32_DE_SIZE;
        uint8_t *buf = malloc(bytes);
        if (!buf) return -ENOMEM;
        if (read_checked(mnt, buf, bytes, mnt->root_dir_offset - mnt->partition_offset) != 0) { free(buf); return -EIO; }
        int r = walk_entries_buf(buf, bytes, callback, context);
        free(buf);
        return r;
    }
    if (dir_cluster < 2) return -EIO;
    uint32_t cur = dir_cluster;
    while (cur >= 2 && cur < FAT32_CHAIN_EOF) {
        uint8_t *cluster_buf = malloc(mnt->cluster_size);
        if (!cluster_buf) return -ENOMEM;
        int64_t got = read_cluster_data(mnt, cur, cluster_buf, mnt->cluster_size, 0);
        if (got < (int64_t)mnt->cluster_size) { free(cluster_buf); return -EIO; }
        int r = walk_entries_buf(cluster_buf, mnt->cluster_size, callback, context);
        free(cluster_buf);
        if (r != 0) return r;
        cur = get_next_cluster(mnt, cur);
    }
    return 0;
}

static int match_lookup(const vfat_dirent_t *dirent, void *context) {
    vfat_lookup_context_t *ctx = context;
    const char *name = dirent->long_name_len > 0 ? dirent->long_name : dirent->short_name;
    size_t len = dirent->long_name_len > 0 ? dirent->long_name_len : dirent->short_name_len;
    if (ctx->component_len == len && memcmp(ctx->component, name, len) == 0) { ctx->found = *dirent; ctx->matched = true; return 1; }
    return 0;
}

static int lookup_child(const vfat_mount_t *mnt, uint32_t dir_cluster, const char *name, size_t name_len, vfat_dirent_t *out) {
    if (!name_len || name_len > VFAT_MAX_COMPONENT) return -ENAMETOOLONG;
    vfat_lookup_context_t ctx = { name, name_len, false, {0} };
    int status = walk_directory(mnt, dir_cluster, (vfat_dir_callback_t)match_lookup, &ctx);
    if (status == 1 && ctx.matched) { *out = ctx.found; return 0; }
    return status < 0 ? status : -ENOENT;
}

static int resolve_path(const vfat_mount_t *mnt, const char *relative, bool follow_final, uint32_t *resolved_cluster, vfat_dirent_t *resolved_dirent) {
    (void)follow_final;
    char work[VFAT_MAX_PATH];
    if (strlen(relative) + 2 > sizeof(work)) return -ENAMETOOLONG;
    work[0] = '/';
    strlcpy(work + 1, relative, sizeof(work) - 1);
    uint32_t current = mnt->root_cluster;
    vfat_dirent_t current_dirent;
    memset(&current_dirent, 0, sizeof(current_dirent));
    current_dirent.first_cluster = mnt->root_cluster;
    current_dirent.attr = FAT32_ATTR_DIRECTORY;
    size_t pos = 0;
    while (work[pos]) {
        while (work[pos] == '/') pos++;
        if (!work[pos]) break;
        size_t start = pos;
        while (work[pos] && work[pos] != '/') pos++;
        size_t end = pos;
        size_t len = end - start;
        if (len == 1 && work[start] == '.') continue;
        if (len == 2 && work[start] == '.' && work[start + 1] == '.') continue;
        vfat_dirent_t child;
        int status = lookup_child(mnt, current, work + start, len, &child);
        if (status < 0) return status;
        bool is_dir = (child.attr & FAT32_ATTR_DIRECTORY) != 0;
        bool is_final = true;
        for (size_t p = end; work[p]; p++) { if (work[p] != '/') { is_final = false; break; } }
        if (!is_final && !is_dir) return -ENOTDIR;
        current = child.first_cluster;
        current_dirent = child;
    }
    if (resolved_cluster) *resolved_cluster = current;
    if (resolved_dirent) *resolved_dirent = current_dirent;
    return 0;
}

static uint8_t compute_sfn_checksum(const uint8_t *sfn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i];
    return sum;
}

static void encode_dos_time_date(uint16_t *out_time, uint16_t *out_date) {
    struct timespec ts = time_get_realtime_ts();
    time_t sec = ts.tv_sec;
    uint32_t days = sec / 86400;
    uint32_t secs = sec % 86400;
    uint32_t year = 1970;
    while (1) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        uint32_t yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        year++;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (leap) mdays[1] = 29;
    uint32_t month = 1;
    for (int i = 0; i < 12; i++) {
        if (days < mdays[i]) break;
        days -= mdays[i];
        month++;
    }
    uint32_t day = days + 1;
    uint32_t hour = secs / 3600;
    uint32_t minute = (secs % 3600) / 60;
    uint32_t second = secs % 60;
    if (year < 1980) { year = 1980; month = 1; day = 1; hour = 0; minute = 0; second = 0; }
    if (year > 2107) year = 2107;
    *out_date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *out_time = (uint16_t)((hour << 11) | (minute << 5) | (second / 2));
}

static bool split_parent(const char *relative, char *parent_out, size_t parent_size, char *leaf_out, size_t leaf_size) {
    size_t len = strlen(relative);
    while (len > 0 && relative[len - 1] == '/') len--;
    size_t slash = len;
    while (slash > 0 && relative[slash - 1] != '/') slash--;
    size_t leaf_len = len - (slash > 0 ? slash : 0);
    if (slash > 0) slash--;
    size_t parent_len = slash;
    while (parent_len > 0 && relative[parent_len - 1] == '/') parent_len--;
    if (parent_len >= parent_size || leaf_len >= leaf_size) return false;
    if (parent_len == 0) parent_out[0] = '\0';
    else { memcpy(parent_out, relative, parent_len); parent_out[parent_len] = '\0'; }
    memcpy(leaf_out, relative + (len - leaf_len), leaf_len);
    leaf_out[leaf_len] = '\0';
    return true;
}

static int resolve_parent(const vfat_mount_t *mnt, const char *relative, uint32_t *out_parent_cluster, char *leaf_out) {
    char parent[VFAT_MAX_PATH];
    char leaf[VFAT_MAX_COMPONENT + 1];
    if (!split_parent(relative, parent, sizeof(parent), leaf, sizeof(leaf))) return -ENAMETOOLONG;
    if (leaf[0] == '\0') return -EINVAL;
    if (strlen(leaf) > VFAT_MAX_COMPONENT) return -ENAMETOOLONG;
    uint32_t parent_cluster = mnt->root_cluster;
    if (parent[0] != '\0') {
        vfat_dirent_t pd;
        int r = resolve_path(mnt, parent, true, &parent_cluster, &pd);
        if (r < 0) return r;
        if (!(pd.attr & FAT32_ATTR_DIRECTORY)) return -ENOTDIR;
    }
    if (out_parent_cluster) *out_parent_cluster = parent_cluster;
    if (leaf_out) strlcpy(leaf_out, leaf, VFAT_MAX_COMPONENT + 1);
    return 0;
}

static bool is_valid_sfn_char(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '-' || c == '@' || c == '^' || c == '_' || c == '`' || c == '{' || c == '}' || c == '~') return true;
    return false;
}

static void build_sfn_candidate(const char *leaf, uint32_t attempt, uint8_t *out_sfn) {
    for (int i = 0; i < 11; i++) out_sfn[i] = ' ';
    char upper[VFAT_MAX_COMPONENT + 1];
    size_t len = strlen(leaf);
    for (size_t i = 0; i < len && i < VFAT_MAX_COMPONENT; i++) {
        char c = leaf[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        upper[i] = c;
    }
    upper[len] = '\0';
    char *dot = strrchr(upper, '.');
    char *name_part = upper;
    size_t name_len = len;
    size_t ext_len = 0;
    char *ext_part = NULL;
    if (dot && dot != upper) {
        name_len = dot - upper;
        ext_part = dot + 1;
        ext_len = len - name_len - 1;
    } else {
        ext_part = NULL;
        ext_len = 0;
    }
    char clean_name[9] = {0};
    size_t cn = 0;
    for (size_t i = 0; i < name_len && cn < 8; i++) {
        char c = name_part[i];
        if (c == '.' || c == ' ') continue;
        if (!is_valid_sfn_char(c)) c = '_';
        clean_name[cn++] = c;
    }
    char clean_ext[4] = {0};
    size_t ce = 0;
    if (ext_part) {
        for (size_t i = 0; i < ext_len && ce < 3; i++) {
            char c = ext_part[i];
            if (c == '.' || c == ' ') continue;
            if (!is_valid_sfn_char(c)) c = '_';
            clean_ext[ce++] = c;
        }
    }
    if (attempt == 0) {
        for (size_t i = 0; i < cn && i < 8; i++) out_sfn[i] = clean_name[i];
        for (size_t i = 0; i < ce && i < 3; i++) out_sfn[8 + i] = clean_ext[i];
        return;
    }
    char suffix[8];
    int slen = 0;
    uint32_t tmp = attempt;
    slen = 0;
    suffix[slen++] = '~';
    char num[6];
    int nlen = 0;
    if (tmp == 0) num[nlen++] = '1';
    else { char rev[6]; int rlen = 0; while (tmp > 0 && rlen < 6) { rev[rlen++] = '0' + (tmp % 10); tmp /= 10; } for (int i = rlen - 1; i >= 0; i--) num[nlen++] = rev[i]; }
    for (int i = 0; i < nlen; i++) suffix[slen++] = num[i];
    size_t keep = 8 - slen;
    if (keep > cn) keep = cn;
    for (size_t i = 0; i < keep; i++) out_sfn[i] = clean_name[i];
    for (int i = 0; i < slen; i++) out_sfn[keep + i] = suffix[i];
    for (size_t i = 0; i < ce && i < 3; i++) out_sfn[8 + i] = clean_ext[i];
}

static bool needs_lfn(const char *leaf, const uint8_t *sfn) {
    char decoded[13];
    size_t dlen = 0;
    decode_short_name(sfn, decoded, &dlen);
    if (strlen(leaf) != dlen) return true;
    for (size_t i = 0; i < dlen; i++) {
        if (leaf[i] != decoded[i]) return true;
    }
    if (strchr(leaf, ' ') || strchr(leaf, '+') || strchr(leaf, ',')) return true;
    return false;
}

static int encode_lfn_entries(const char *leaf, const uint8_t *sfn, uint8_t *out_buf, uint32_t *out_count) {
    uint16_t utf16[130];
    size_t ulen = 0;
    const uint8_t *p = (const uint8_t *)leaf;
    while (*p && ulen < 128) {
        uint32_t cp = 0;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { p++; continue; }
        if (cp >= 0x10000) continue;
        utf16[ulen++] = (uint16_t)cp;
    }
    if (ulen == 0) { *out_count = 0; return 0; }
    uint8_t sfn_tmp[11];
    memcpy(sfn_tmp, sfn, 11);
    char decoded[13];
    size_t dlen;
    decode_short_name(sfn_tmp, decoded, &dlen);
    bool use_lfn = needs_lfn(leaf, sfn);
    if (!use_lfn) { *out_count = 0; return 0; }
    uint32_t needed = (ulen + 12) / 13;
    if (needed > VFAT_MAX_LFN_ENTRIES) return -ENAMETOOLONG;
    uint8_t chksum = compute_sfn_checksum(sfn);
    for (uint32_t idx = 0; idx < needed; idx++) {
        uint8_t *e = out_buf + idx * FAT32_DE_SIZE;
        memset(e, 0, FAT32_DE_SIZE);
        uint32_t order = needed - idx;
        if (idx == 0) order |= 0x40;
        e[FAT32_LFN_ORDER] = (uint8_t)order;
        e[FAT32_LFN_ATTR] = FAT32_ATTR_LFN;
        e[FAT32_LFN_CHECKSUM] = chksum;
        for (int i = 0; i < 5; i++) { uint32_t pos = idx * 13 + i; uint16_t v = pos < ulen ? utf16[pos] : (pos == ulen ? 0x0000 : 0xFFFF); write_le16(e + FAT32_LFN_NAME1 + i * 2, v); }
        for (int i = 0; i < 6; i++) { uint32_t pos = idx * 13 + 5 + i; uint16_t v = pos < ulen ? utf16[pos] : (pos == ulen ? 0x0000 : 0xFFFF); write_le16(e + FAT32_LFN_NAME2 + i * 2, v); }
        for (int i = 0; i < 2; i++) { uint32_t pos = idx * 13 + 11 + i; uint16_t v = pos < ulen ? utf16[pos] : (pos == ulen ? 0x0000 : 0xFFFF); write_le16(e + FAT32_LFN_NAME3 + i * 2, v); }
    }
    *out_count = needed;
    return 0;
}

static int find_free_slots(const vfat_mount_t *mnt, uint32_t dir_cluster, uint32_t needed, uint64_t *out_byte_offset, uint32_t *out_cluster) {
    if (needed == 0) needed = 1;
    if (mnt->fat_bits == 16 && dir_cluster == 0) {
        uint32_t total = mnt->root_dir_entries;
        uint8_t *buf = malloc(total * FAT32_DE_SIZE);
        if (!buf) return -ENOMEM;
        int r = read_checked(mnt, buf, total * FAT32_DE_SIZE, mnt->root_dir_offset - mnt->partition_offset);
        if (r < 0) { free(buf); return r; }
        uint32_t run = 0;
        for (uint32_t i = 0; i < total; i++) {
            uint8_t *e = buf + i * FAT32_DE_SIZE;
            if (e[0] == 0x00 || e[0] == 0xE5) run++;
            else run = 0;
            if (run >= needed) {
                uint32_t start = i + 1 - needed;
                *out_byte_offset = (uint64_t)start * FAT32_DE_SIZE;
                *out_cluster = 0;
                free(buf);
                return 0;
            }
            if (e[0] == 0x00) break;
        }
        free(buf);
        return -ENOSPC;
    }
    uint32_t cur = dir_cluster;
    uint64_t base = 0;
    while (cur >= 2 && cur < FAT32_CHAIN_EOF) {
        uint8_t *buf = malloc(mnt->cluster_size);
        if (!buf) return -ENOMEM;
        int r = read_cluster_data(mnt, cur, buf, mnt->cluster_size, 0);
        if (r < 0) { free(buf); return r; }
        uint32_t entries = mnt->cluster_size / FAT32_DE_SIZE;
        uint32_t run = 0;
        uint32_t run_start = 0;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = buf + i * FAT32_DE_SIZE;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                if (run == 0) run_start = i;
                run++;
                if (run >= needed) {
                    *out_byte_offset = base + (uint64_t)run_start * FAT32_DE_SIZE;
                    *out_cluster = cur;
                    free(buf);
                    return 0;
                }
            } else run = 0;
            if (e[0] == 0x00) {
                uint32_t remaining = entries - i - 1;
                if (run + remaining >= needed && run > 0) {
                }
                break;
            }
        }
        if (buf[0] == 0x00 && false) {}
        free(buf);
        uint32_t nxt = get_next_cluster(mnt, cur);
        if (nxt >= FAT32_CHAIN_EOF) break;
        base += mnt->cluster_size;
        cur = nxt;
    }
    uint32_t new_cluster = 0;
    int r = alloc_chain(mnt, 1, &new_cluster);
    if (r < 0) return r;
    uint32_t last = 0;
    get_last_cluster(mnt, dir_cluster, &last);
    set_next_cluster(mnt, last, new_cluster);
    *out_byte_offset = base + mnt->cluster_size;
    *out_cluster = new_cluster;
    return 0;
}

static int write_dir_entry_at(const vfat_mount_t *mnt, uint32_t dir_cluster, uint64_t byte_offset, const uint8_t *entry) {
    if (mnt->fat_bits == 16 && dir_cluster == 0) {
        uint64_t off = mnt->root_dir_offset - mnt->partition_offset + byte_offset;
        return write_checked(mnt, entry, FAT32_DE_SIZE, off);
    }
    uint32_t idx = (uint32_t)(byte_offset / mnt->cluster_size);
    uint64_t in_off = byte_offset % mnt->cluster_size;
    uint32_t cluster = 0;
    int r = get_cluster_at_index(mnt, dir_cluster, idx, &cluster);
    if (r < 0) return r;
    return write_cluster_data(mnt, cluster, entry, FAT32_DE_SIZE, in_off);
}

static int read_dir_entry_at(const vfat_mount_t *mnt, uint32_t dir_cluster, uint64_t byte_offset, uint8_t *out) {
    if (mnt->fat_bits == 16 && dir_cluster == 0) {
        uint64_t off = mnt->root_dir_offset - mnt->partition_offset + byte_offset;
        return read_checked(mnt, out, FAT32_DE_SIZE, off);
    }
    uint32_t idx = (uint32_t)(byte_offset / mnt->cluster_size);
    uint64_t in_off = byte_offset % mnt->cluster_size;
    uint32_t cluster = 0;
    int r = get_cluster_at_index(mnt, dir_cluster, idx, &cluster);
    if (r < 0) return r;
    int64_t got = read_cluster_data(mnt, cluster, out, FAT32_DE_SIZE, in_off);
    return got == FAT32_DE_SIZE ? 0 : -EIO;
}

static int locate_entry(const vfat_mount_t *mnt, uint32_t dir_cluster, const char *name, size_t name_len, uint64_t *out_offset, vfat_dirent_t *out_dirent) {
    if (mnt->fat_bits == 16 && dir_cluster == 0) {
        uint32_t total = mnt->root_dir_entries;
        uint8_t *buf = malloc(total * FAT32_DE_SIZE);
        if (!buf) return -ENOMEM;
        int r = read_checked(mnt, buf, total * FAT32_DE_SIZE, mnt->root_dir_offset - mnt->partition_offset);
        if (r < 0) { free(buf); return r; }
        uint8_t lfn_name[VFAT_MAX_NAME * 3];
        size_t lfn_len = 0;
        uint8_t lfn_chksum = 0;
        bool have_lfn = false;
        for (uint32_t i = 0; i < total; i++) {
            uint8_t *e = buf + i * FAT32_DE_SIZE;
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5) { lfn_len = 0; have_lfn = false; continue; }
            if (e[FAT32_DE_ATTR] == FAT32_ATTR_LFN) {
                uint8_t order = e[FAT32_LFN_ORDER];
                uint8_t cs = e[FAT32_LFN_CHECKSUM];
                if (order & 0x40) { lfn_len = 0; lfn_chksum = cs; }
                else if (cs != lfn_chksum) { lfn_len = 0; continue; }
                decode_lfn_name(e, (char *)lfn_name, &lfn_len, order);
                if (!(order & 0x40) && order == 1) { lfn_name[lfn_len] = '\0'; have_lfn = true; }
                continue;
            }
            if (e[FAT32_DE_ATTR] == FAT32_ATTR_VOLUME_ID) { lfn_len = 0; have_lfn = false; continue; }
            vfat_dirent_t de;
            memset(&de, 0, sizeof(de));
            de.first_cluster = (uint32_t)read_le16(e + FAT32_DE_FSTCLUS_HI) << 16 | read_le16(e + FAT32_DE_FSTCLUS_LO);
            de.file_size = read_le32(e + FAT32_DE_FILESIZE);
            de.attr = e[FAT32_DE_ATTR];
            if (have_lfn) { memcpy(de.long_name, lfn_name, lfn_len + 1); de.long_name_len = lfn_len; }
            decode_short_name(e, de.short_name, &de.short_name_len);
            const char *n = de.long_name_len > 0 ? de.long_name : de.short_name;
            size_t l = de.long_name_len > 0 ? de.long_name_len : de.short_name_len;
            if (l == name_len && memcmp(n, name, l) == 0) {
                if (out_offset) *out_offset = (uint64_t)i * FAT32_DE_SIZE;
                if (out_dirent) *out_dirent = de;
                free(buf);
                return 0;
            }
            lfn_len = 0; have_lfn = false;
        }
        free(buf);
        return -ENOENT;
    }
    uint32_t cur = dir_cluster;
    uint64_t base = 0;
    uint8_t lfn_name[VFAT_MAX_NAME * 3];
    size_t lfn_len = 0;
    uint8_t lfn_chksum = 0;
    bool have_lfn = false;
    while (cur >= 2 && cur < FAT32_CHAIN_EOF) {
        uint8_t *buf = malloc(mnt->cluster_size);
        if (!buf) return -ENOMEM;
        int64_t got = read_cluster_data(mnt, cur, buf, mnt->cluster_size, 0);
        if (got < 0) { free(buf); return -EIO; }
        uint32_t entries = mnt->cluster_size / FAT32_DE_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = buf + i * FAT32_DE_SIZE;
            if (e[0] == 0x00) { free(buf); return -ENOENT; }
            if (e[0] == 0xE5) { lfn_len = 0; have_lfn = false; continue; }
            if (e[FAT32_DE_ATTR] == FAT32_ATTR_LFN) {
                if (e[FAT32_LFN_ORDER] & 0x40) { lfn_len = 0; lfn_chksum = e[FAT32_LFN_CHECKSUM]; }
                else if (e[FAT32_LFN_CHECKSUM] != lfn_chksum) { lfn_len = 0; continue; }
                decode_lfn_name(e, (char *)lfn_name, &lfn_len, e[FAT32_LFN_ORDER]);
                if (!(e[FAT32_LFN_ORDER] & 0x40) && (e[FAT32_LFN_ORDER] & 0x1F) == 1) { lfn_name[lfn_len] = '\0'; have_lfn = true; }
                continue;
            }
            if (e[FAT32_DE_ATTR] == FAT32_ATTR_VOLUME_ID) { lfn_len = 0; have_lfn = false; continue; }
            vfat_dirent_t de;
            memset(&de, 0, sizeof(de));
            de.first_cluster = (uint32_t)read_le16(e + FAT32_DE_FSTCLUS_HI) << 16 | read_le16(e + FAT32_DE_FSTCLUS_LO);
            de.file_size = read_le32(e + FAT32_DE_FILESIZE);
            de.attr = e[FAT32_DE_ATTR];
            if (have_lfn) { memcpy(de.long_name, lfn_name, lfn_len + 1); de.long_name_len = lfn_len; }
            decode_short_name(e, de.short_name, &de.short_name_len);
            const char *n = de.long_name_len > 0 ? de.long_name : de.short_name;
            size_t l = de.long_name_len > 0 ? de.long_name_len : de.short_name_len;
            if (l == name_len && memcmp(n, name, l) == 0) {
                if (out_offset) *out_offset = base + i * FAT32_DE_SIZE;
                if (out_dirent) *out_dirent = de;
                free(buf);
                return 0;
            }
            lfn_len = 0; have_lfn = false;
        }
        free(buf);
        uint32_t nxt = get_next_cluster(mnt, cur);
        if (nxt >= FAT32_CHAIN_EOF) break;
        base += mnt->cluster_size;
        cur = nxt;
    }
    return -ENOENT;
}

static int update_dir_entry(const vfat_mount_t *mnt, uint32_t dir_cluster, uint64_t offset, uint32_t first_cluster, uint32_t file_size) {
    uint8_t entry[FAT32_DE_SIZE];
    int r = read_dir_entry_at(mnt, dir_cluster, offset, entry);
    if (r < 0) return r;
    write_le16(entry + FAT32_DE_FSTCLUS_HI, (first_cluster >> 16) & 0xFFFF);
    write_le16(entry + FAT32_DE_FSTCLUS_LO, first_cluster & 0xFFFF);
    write_le32(entry + FAT32_DE_FILESIZE, file_size);
    uint16_t dos_time, dos_date;
    encode_dos_time_date(&dos_time, &dos_date);
    write_le16(entry + FAT32_DE_WRT_TIME, dos_time);
    write_le16(entry + FAT32_DE_WRT_DATE, dos_date);
    write_le16(entry + FAT32_DE_LST_ACC_DATE, dos_date);
    return write_dir_entry_at(mnt, dir_cluster, offset, entry);
}

static int create_dir_entry(const vfat_mount_t *mnt, uint32_t dir_cluster, const char *leaf, uint32_t first_cluster, uint32_t file_size, uint8_t attr) {
    uint8_t sfn[11];
    uint8_t lfn_buf[VFAT_MAX_LFN_ENTRIES * FAT32_DE_SIZE];
    uint32_t lfn_count = 0;
    for (uint32_t attempt = 0; attempt < 100; attempt++) {
        build_sfn_candidate(leaf, attempt, sfn);
        uint64_t dummy;
        bool exists = locate_entry(mnt, dir_cluster, leaf, strlen(leaf), &dummy, NULL) == 0;
        if (exists && attempt == 0) continue;
        bool sfn_exists = false;
        if (mnt->fat_bits == 16 && dir_cluster == 0) {
            uint32_t total = mnt->root_dir_entries;
            uint8_t *buf = malloc(total * FAT32_DE_SIZE);
            if (buf) {
                read_checked(mnt, buf, total * FAT32_DE_SIZE, mnt->root_dir_offset - mnt->partition_offset);
                for (uint32_t i = 0; i < total; i++) {
                    uint8_t *e = buf + i * FAT32_DE_SIZE;
                    if (e[0] == 0x00 || e[0] == 0xE5) continue;
                    if (e[FAT32_DE_ATTR] == FAT32_ATTR_LFN || e[FAT32_DE_ATTR] == FAT32_ATTR_VOLUME_ID) continue;
                    if (memcmp(e, sfn, 11) == 0) sfn_exists = true;
                }
                free(buf);
            }
        } else {
            uint32_t cur = dir_cluster;
            while (cur >= 2 && cur < FAT32_CHAIN_EOF && !sfn_exists) {
                uint8_t *buf = malloc(mnt->cluster_size);
                if (!buf) break;
                read_cluster_data(mnt, cur, buf, mnt->cluster_size, 0);
                uint32_t entries = mnt->cluster_size / FAT32_DE_SIZE;
                for (uint32_t i = 0; i < entries; i++) {
                    uint8_t *e = buf + i * FAT32_DE_SIZE;
                    if (e[0] == 0x00) break;
                    if (e[0] == 0xE5) continue;
                    if (e[FAT32_DE_ATTR] == FAT32_ATTR_LFN || e[FAT32_DE_ATTR] == FAT32_ATTR_VOLUME_ID) continue;
                    if (memcmp(e, sfn, 11) == 0) sfn_exists = true;
                }
                free(buf);
                cur = get_next_cluster(mnt, cur);
            }
        }
        if (sfn_exists) continue;
        int r = encode_lfn_entries(leaf, sfn, lfn_buf, &lfn_count);
        if (r < 0) return r;
        break;
    }
    uint32_t needed = lfn_count + 1;
    uint64_t off = 0;
    uint32_t target_cluster = 0;
    int r = find_free_slots(mnt, dir_cluster, needed, &off, &target_cluster);
    if (r < 0) return r;
    for (uint32_t i = 0; i < lfn_count; i++) {
        uint8_t *e = lfn_buf + i * FAT32_DE_SIZE;
        uint64_t cur_off = off + i * FAT32_DE_SIZE;
        r = write_dir_entry_at(mnt, dir_cluster, cur_off, e);
        if (r < 0) return r;
    }
    uint8_t sfn_entry[FAT32_DE_SIZE];
    memset(sfn_entry, 0, FAT32_DE_SIZE);
    memcpy(sfn_entry, sfn, 11);
    sfn_entry[FAT32_DE_ATTR] = attr;
    uint16_t dos_time, dos_date;
    encode_dos_time_date(&dos_time, &dos_date);
    write_le16(sfn_entry + FAT32_DE_CRT_TIME, dos_time);
    write_le16(sfn_entry + FAT32_DE_CRT_DATE, dos_date);
    write_le16(sfn_entry + FAT32_DE_LST_ACC_DATE, dos_date);
    write_le16(sfn_entry + FAT32_DE_WRT_TIME, dos_time);
    write_le16(sfn_entry + FAT32_DE_WRT_DATE, dos_date);
    write_le16(sfn_entry + FAT32_DE_FSTCLUS_HI, (first_cluster >> 16) & 0xFFFF);
    write_le16(sfn_entry + FAT32_DE_FSTCLUS_LO, first_cluster & 0xFFFF);
    write_le32(sfn_entry + FAT32_DE_FILESIZE, file_size);
    uint64_t sfn_off = off + lfn_count * FAT32_DE_SIZE;
    return write_dir_entry_at(mnt, dir_cluster, sfn_off, sfn_entry);
}

static bool probe_fat32_boot(const uint8_t *boot) {
    uint16_t bps = read_le16(boot + FAT32_BS_BYTES_PER_SEC);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return false;
    uint8_t spc = boot[FAT32_BS_SEC_PERCLUS];
    if (!spc || (spc & (spc - 1))) return false;
    if (!boot[FAT32_BS_NUM_FATS]) return false;
    uint32_t fsz = read_le16(boot + FAT32_BS_FAT_SZ_16);
    if (!fsz) fsz = read_le32(boot + FAT32_BS_FAT_SZ_32);
    if (!fsz) return false;
    return true;
}

static bool match_gpt_fat32(const uint8_t *type_guid) {
    for (int i = 0; i < 16; i++) if (type_guid[i] != vfat_gpt_guid[i]) return false;
    return true;
}

static int find_fat32_partition(const char *dev, uint8_t *boot, uint64_t *part_off) {
    uint8_t mbr[512];
    if (read_device(dev, mbr, 512, 0) != 512) return -EIO;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -EINVAL;
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = mbr + 446 + i * 16;
        uint8_t type = entry[4];
        if (type != 0x04 && type != 0x06 && type != 0x0B && type != 0x0C) continue;
        uint32_t lba = read_le32(entry + 8);
        uint32_t secs = read_le32(entry + 12);
        if (!lba || !secs) continue;
        uint64_t off = (uint64_t)lba * 512;
        if (read_device(dev, boot, 512, off) != 512) continue;
        if (probe_fat32_boot(boot)) { *part_off = off; return 0; }
    }
    if (mbr[446 + 4] == 0xEE) {
        uint8_t gpt_sec[512];
        if (read_device(dev, gpt_sec, 512, 512) != 512) return -EINVAL;
        uint64_t sig;
        memcpy(&sig, gpt_sec, 8);
        if (sig != 0x5452415020494645ULL) return -EINVAL;
        uint32_t entry_cnt = *(uint32_t *)(gpt_sec + 80);
        uint32_t entry_sz = *(uint32_t *)(gpt_sec + 84);
        uint64_t entry_lba;
        memcpy(&entry_lba, gpt_sec + 72, 8);
        if (!entry_cnt || !entry_sz || entry_sz < 128) return -EINVAL;
        uint32_t max_entries = entry_cnt > 128 ? 128 : entry_cnt;
        uint32_t total_bytes = max_entries * entry_sz;
        uint8_t *entries = malloc(total_bytes);
        if (!entries) return -ENOMEM;
        uint64_t entries_off = entry_lba * 512;
        if (read_device(dev, entries, total_bytes, entries_off) != total_bytes) { free(entries); return -EINVAL; }
        for (uint32_t i = 0; i < max_entries; i++) {
            uint8_t *e = entries + (uint64_t)i * entry_sz;
            bool empty = true;
            for (int j = 0; j < 16; j++) if (e[j]) { empty = false; break; }
            if (empty) continue;
            if (!match_gpt_fat32(e)) continue;
            uint64_t first_lba;
            memcpy(&first_lba, e + 32, 8);
            if (first_lba < 2) continue;
            uint64_t off = first_lba * 512;
            if (read_device(dev, e, 512, off) != 512) {
                if (read_device(dev, boot, 512, off) != 512) continue;
            } else memcpy(boot, e, 512);
            if (probe_fat32_boot(boot)) { free(entries); *part_off = off; return 0; }
        }
        free(entries);
    }
    return -EINVAL;
}

int mount_vfat(const char *source, const char *target, unsigned long flags, const char *data) {
    (void)flags; (void)data;
    const char *dev = get_device_name(source);
    if (!dev || !*dev || strlen(dev) > 64 || !target || target[0] != '/' || strlen(target) > 63) return -EINVAL;
    uint64_t device_size;
    int status = get_block_device_size(dev, &device_size);
    if (status < 0) return status;
    if (device_size < 512) return -EINVAL;
    uint8_t boot[512];
    uint64_t got = read_device(dev, boot, 512, 0);
    if (got != 512) return -EIO;
    uint64_t part_off = 0;
    if (!probe_fat32_boot(boot)) {
        status = find_fat32_partition(dev, boot, &part_off);
        if (status < 0) return status;
    }
    uint16_t bytes_per_sec = read_le16(boot + FAT32_BS_BYTES_PER_SEC);
    uint8_t sec_per_clus = boot[FAT32_BS_SEC_PERCLUS];
    uint16_t rsvd = read_le16(boot + FAT32_BS_RSVD_SEC_CNT);
    uint8_t num_fats = boot[FAT32_BS_NUM_FATS];
    uint16_t root_ent_cnt = read_le16(boot + FAT32_BS_ROOT_ENT_CNT);
    uint32_t fat_sz = read_le16(boot + FAT32_BS_FAT_SZ_16);
    if (!fat_sz) fat_sz = read_le32(boot + FAT32_BS_FAT_SZ_32);
    uint32_t rootclus = read_le32(boot + FAT32_BS_ROOTCLUS);
    uint32_t total_sectors;
    uint16_t tot_sec_16 = read_le16(boot + FAT32_BS_TOT_SEC_16);
    if (tot_sec_16) total_sectors = tot_sec_16;
    else total_sectors = read_le32(boot + FAT32_BS_TOT_SEC_32);
    if (!total_sectors) return -EINVAL;
    uint64_t root_dir_bytes = (uint32_t)root_ent_cnt * 32;
    uint64_t root_dir_secs = (root_dir_bytes + bytes_per_sec - 1) / bytes_per_sec;
    uint64_t fat_area = (uint64_t)(rsvd + (uint32_t)num_fats * fat_sz) * bytes_per_sec;
    uint64_t data_start_no_root = fat_area;
    uint64_t ds_nr = (uint64_t)total_sectors - data_start_no_root / bytes_per_sec;
    uint32_t tc_nr = (uint32_t)(ds_nr / sec_per_clus);
    bool is_fat16 = tc_nr <= 65525 && tc_nr > 4085;
    if (!is_fat16 && rootclus < 2 && total_sectors > 0) return -EINVAL;
    uint64_t data_start = fat_area + (is_fat16 ? root_dir_secs * bytes_per_sec : 0);
    uint64_t data_sectors = (uint64_t)total_sectors - data_start / bytes_per_sec;
    uint32_t total_clusters = (uint32_t)(data_sectors / sec_per_clus);
    if (total_clusters <= 4085) return -EINVAL;
    if (total_clusters <= 65525) is_fat16 = true;
    if (!is_fat16 && rootclus < 2) return -EINVAL;
    vfat_mount_t probe;
    memset(&probe, 0, sizeof(probe));
    strlcpy(probe.device, dev, sizeof(probe.device));
    probe.device_size = device_size;
    probe.bytes_per_sector = bytes_per_sec;
    probe.sectors_per_cluster = sec_per_clus;
    probe.cluster_size = (uint32_t)bytes_per_sec * sec_per_clus;
    probe.reserved_sectors = rsvd;
    probe.num_fats = num_fats;
    probe.fat_size = fat_sz;
    probe.root_cluster = rootclus;
    probe.total_clusters = total_clusters;
    probe.partition_offset = part_off;
    probe.fat_offset = (uint64_t)rsvd * bytes_per_sec;
    probe.data_offset = data_start;
    probe.fat_bits = is_fat16 ? 16 : 32;
    if (is_fat16) probe.root_cluster = 0;
    probe.root_dir_entries = root_ent_cnt;
    probe.root_dir_offset = part_off + fat_area;
    probe.fsinfo_sector = read_le16(boot + FAT32_BS_FSI);
    probe.volume_id = read_le32(boot + FAT32_BS_VOL_ID);
    char normalized[64];
    strlcpy(normalized, target, sizeof(normalized));
    size_t target_len = strlen(normalized);
    while (target_len > 1 && normalized[target_len - 1] == '/') normalized[--target_len] = '\0';
    strlcpy(probe.target, normalized, sizeof(probe.target));
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    int slot = -1;
    for (int i = 0; i < VFAT_MAX_MOUNTS; i++) {
        if (vfat_mounts[i].active && strcmp(vfat_mounts[i].target, normalized) == 0) { spin_unlock_irqrestore(&vfat_lock, irq); return -EBUSY; }
        if (!vfat_mounts[i].active && slot < 0) slot = i;
    }
    if (slot < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return -ENOSPC; }
    vfat_mounts[slot] = probe;
    vfat_mounts[slot].active = true;
    spin_unlock_irqrestore(&vfat_lock, irq);
    return 0;
}

int unmount_vfat(const char *target) {
    if (!target) return -EINVAL;
    char normalized[64];
    if (strlen(target) >= sizeof(normalized)) return -ENAMETOOLONG;
    strlcpy(normalized, target, sizeof(normalized));
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/') normalized[--length] = '\0';
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    for (int i = 0; i < VFAT_MAX_MOUNTS; i++) {
        if (vfat_mounts[i].active && strcmp(vfat_mounts[i].target, normalized) == 0) {
            memset(&vfat_mounts[i], 0, sizeof(vfat_mounts[i]));
            spin_unlock_irqrestore(&vfat_lock, irq);
            return 0;
        }
    }
    spin_unlock_irqrestore(&vfat_lock, irq);
    return -ENOENT;
}

bool check_vfat_path(const char *path) { return path && find_mount(path, NULL) != NULL; }

int stat_vfat(const char *path, struct stat *st, bool follow) {
    if (!path || !st) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t cluster;
    vfat_dirent_t dirent;
    int status = resolve_path(mnt, relative, follow, &cluster, &dirent);
    if (status < 0) return status;
    memset(st, 0, sizeof(*st));
    st->st_ino = dirent.first_cluster ? dirent.first_cluster : 2;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_size = dirent.file_size;
    st->st_blksize = mnt->cluster_size;
    st->st_blocks = (dirent.file_size + 511) / 512;
    if (dirent.attr & FAT32_ATTR_DIRECTORY) st->st_mode = S_IFDIR | 0755;
    else if (dirent.attr & FAT32_ATTR_READ_ONLY) st->st_mode = S_IFREG | 0555;
    else st->st_mode = S_IFREG | 0755;
    return 0;
}

int64_t read_vfat(const char *path, void *buffer, uint64_t count, uint64_t offset) {
    if (!path || (!buffer && count)) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t cluster;
    vfat_dirent_t dirent;
    int status = resolve_path(mnt, relative, true, &cluster, &dirent);
    if (status < 0) return status;
    if (dirent.attr & FAT32_ATTR_DIRECTORY) return -EISDIR;
    if (dirent.first_cluster < 2) return 0;
    if (offset >= dirent.file_size) return 0;
    if (offset + count > dirent.file_size) count = dirent.file_size - offset;
    return read_chain(mnt, dirent.first_cluster, buffer, count, offset, dirent.file_size);
}

int64_t write_vfat(const char *path, const void *buffer, uint64_t count, uint64_t offset) {
    if (!path || (!buffer && count)) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    if (relative[0] == '\0') return -EISDIR;
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    uint32_t parent_cluster = 0;
    char leaf[VFAT_MAX_COMPONENT + 1];
    int r = resolve_parent(mnt, relative, &parent_cluster, leaf);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint64_t entry_offset = 0;
    vfat_dirent_t de;
    bool exists = locate_entry(mnt, parent_cluster, leaf, strlen(leaf), &entry_offset, &de) == 0;
    if (!exists) { spin_unlock_irqrestore(&vfat_lock, irq); return -ENOENT; }
    if (de.attr & FAT32_ATTR_DIRECTORY) { spin_unlock_irqrestore(&vfat_lock, irq); return -EISDIR; }
    if (de.attr & FAT32_ATTR_READ_ONLY) { spin_unlock_irqrestore(&vfat_lock, irq); return -EACCES; }
    uint64_t new_end = offset + count;
    uint64_t new_size = de.file_size > new_end ? de.file_size : new_end;
    if (new_size > 0xFFFFFFFFULL) { spin_unlock_irqrestore(&vfat_lock, irq); return -EFBIG; }
    uint32_t needed_clusters = new_size == 0 ? 0 : (uint32_t)((new_size + mnt->cluster_size - 1) / mnt->cluster_size);
    uint32_t first = de.first_cluster;
    uint32_t have_clusters = 0;
    if (first >= 2) count_chain(mnt, first, &have_clusters);
    if (needed_clusters > have_clusters) {
        uint32_t new_first = first;
        r = extend_chain(mnt, first, needed_clusters, &new_first);
        if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
        first = new_first;
        if (have_clusters == 0) {
            uint8_t e[FAT32_DE_SIZE];
            read_dir_entry_at(mnt, parent_cluster, entry_offset, e);
            write_le16(e + FAT32_DE_FSTCLUS_HI, (first >> 16) & 0xFFFF);
            write_le16(e + FAT32_DE_FSTCLUS_LO, first & 0xFFFF);
            write_dir_entry_at(mnt, parent_cluster, entry_offset, e);
            de.first_cluster = first;
        }
        if (offset > de.file_size) {
            uint64_t gap_start = de.file_size;
            uint64_t gap_len = offset - gap_start;
            uint8_t *zero = malloc(gap_len > 4096 ? 4096 : gap_len);
            if (zero) {
                memset(zero, 0, gap_len > 4096 ? 4096 : gap_len);
                uint64_t off = gap_start;
                while (off < offset) {
                    uint64_t chunk = offset - off;
                    if (chunk > 4096) chunk = 4096;
                    write_chain(mnt, first, zero, chunk, off);
                    off += chunk;
                }
                free(zero);
            }
        }
    }
    if (count > 0 && first >= 2) {
        r = (int)write_chain(mnt, first, buffer, count, offset);
        if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    }
    if (new_size != de.file_size || first != de.first_cluster) {
        update_dir_entry(mnt, parent_cluster, entry_offset, first, (uint32_t)new_size);
    } else {
        uint8_t e[FAT32_DE_SIZE];
        read_dir_entry_at(mnt, parent_cluster, entry_offset, e);
        uint16_t dos_time, dos_date;
        encode_dos_time_date(&dos_time, &dos_date);
        write_le16(e + FAT32_DE_WRT_TIME, dos_time);
        write_le16(e + FAT32_DE_WRT_DATE, dos_date);
        write_dir_entry_at(mnt, parent_cluster, entry_offset, e);
    }
    spin_unlock_irqrestore(&vfat_lock, irq);
    return (int64_t)count;
}

int truncate_vfat(const char *path, uint64_t length) {
    if (!path) return -EINVAL;
    if (length > 0xFFFFFFFFULL) return -EFBIG;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    if (relative[0] == '\0') return -EISDIR;
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    uint32_t parent_cluster = 0;
    char leaf[VFAT_MAX_COMPONENT + 1];
    int r = resolve_parent(mnt, relative, &parent_cluster, leaf);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint64_t entry_offset = 0;
    vfat_dirent_t de;
    r = locate_entry(mnt, parent_cluster, leaf, strlen(leaf), &entry_offset, &de);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    if (de.attr & FAT32_ATTR_DIRECTORY) { spin_unlock_irqrestore(&vfat_lock, irq); return -EISDIR; }
    if (de.attr & FAT32_ATTR_READ_ONLY) { spin_unlock_irqrestore(&vfat_lock, irq); return -EACCES; }
    uint32_t needed = length == 0 ? 0 : (uint32_t)((length + mnt->cluster_size - 1) / mnt->cluster_size);
    uint32_t first = de.first_cluster;
    uint32_t new_first = first;
    r = truncate_chain(mnt, first, needed, &new_first);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    if (length > de.file_size && new_first >= 2) {
        uint64_t gap = length - de.file_size;
        uint8_t *zero = malloc(gap > 4096 ? 4096 : gap);
        if (zero) {
            memset(zero, 0, gap > 4096 ? 4096 : gap);
            uint64_t off = de.file_size;
            while (off < length) {
                uint64_t chunk = length - off;
                if (chunk > 4096) chunk = 4096;
                write_chain(mnt, new_first, zero, chunk, off);
                off += chunk;
            }
            free(zero);
        }
    }
    update_dir_entry(mnt, parent_cluster, entry_offset, new_first, (uint32_t)length);
    spin_unlock_irqrestore(&vfat_lock, irq);
    return 0;
}

int create_vfat(const char *path, mode_t mode) {
    if (!path) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    if (relative[0] == '\0') return -EEXIST;
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    uint32_t parent_cluster = 0;
    char leaf[VFAT_MAX_COMPONENT + 1];
    int r = resolve_parent(mnt, relative, &parent_cluster, leaf);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint64_t dummy = 0;
    if (locate_entry(mnt, parent_cluster, leaf, strlen(leaf), &dummy, NULL) == 0) { spin_unlock_irqrestore(&vfat_lock, irq); return -EEXIST; }
    uint8_t attr = FAT32_ATTR_ARCHIVE;
    if ((mode & 0222) == 0) attr |= FAT32_ATTR_READ_ONLY;
    r = create_dir_entry(mnt, parent_cluster, leaf, 0, 0, attr);
    spin_unlock_irqrestore(&vfat_lock, irq);
    return r;
}

int mkdir_vfat(const char *path, mode_t mode) {
    (void)mode;
    if (!path) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    if (relative[0] == '\0') return -EEXIST;
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    uint32_t parent_cluster = 0;
    char leaf[VFAT_MAX_COMPONENT + 1];
    int r = resolve_parent(mnt, relative, &parent_cluster, leaf);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint64_t dummy = 0;
    if (locate_entry(mnt, parent_cluster, leaf, strlen(leaf), &dummy, NULL) == 0) { spin_unlock_irqrestore(&vfat_lock, irq); return -EEXIST; }
    uint32_t new_cluster = 0;
    r = alloc_chain(mnt, 1, &new_cluster);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint8_t *zero = malloc(mnt->cluster_size);
    if (zero) {
        memset(zero, 0, mnt->cluster_size);
        write_cluster_data(mnt, new_cluster, zero, mnt->cluster_size, 0);
        free(zero);
    }
    uint64_t cluster_sector = mnt->data_offset / mnt->bytes_per_sector + (uint64_t)(new_cluster - 2) * mnt->sectors_per_cluster;
    uint64_t byte_off = cluster_sector * mnt->bytes_per_sector;
    uint8_t entry[FAT32_DE_SIZE];
    memset(entry, 0, FAT32_DE_SIZE);
    memcpy(entry, ".          ", 11);
    entry[0] = '.';
    for (int i = 1; i < 11; i++) entry[i] = ' ';
    entry[FAT32_DE_ATTR] = FAT32_ATTR_DIRECTORY;
    write_le16(entry + FAT32_DE_FSTCLUS_HI, (new_cluster >> 16) & 0xFFFF);
    write_le16(entry + FAT32_DE_FSTCLUS_LO, new_cluster & 0xFFFF);
    uint16_t dos_time, dos_date;
    encode_dos_time_date(&dos_time, &dos_date);
    write_le16(entry + FAT32_DE_CRT_TIME, dos_time);
    write_le16(entry + FAT32_DE_CRT_DATE, dos_date);
    write_le16(entry + FAT32_DE_WRT_TIME, dos_time);
    write_le16(entry + FAT32_DE_WRT_DATE, dos_date);
    write_le16(entry + FAT32_DE_LST_ACC_DATE, dos_date);
    write_checked(mnt, entry, FAT32_DE_SIZE, byte_off);
    memset(entry, 0, FAT32_DE_SIZE);
    memcpy(entry, "..         ", 11);
    entry[0] = '.'; entry[1] = '.';
    for (int i = 2; i < 11; i++) entry[i] = ' ';
    entry[FAT32_DE_ATTR] = FAT32_ATTR_DIRECTORY;
    uint32_t parent_for_dotdot = parent_cluster == 0 ? 0 : parent_cluster;
    if (parent_cluster == mnt->root_cluster && mnt->fat_bits == 32) parent_for_dotdot = mnt->root_cluster;
    else if (mnt->fat_bits == 16 && parent_cluster == 0) parent_for_dotdot = 0;
    write_le16(entry + FAT32_DE_FSTCLUS_HI, (parent_for_dotdot >> 16) & 0xFFFF);
    write_le16(entry + FAT32_DE_FSTCLUS_LO, parent_for_dotdot & 0xFFFF);
    write_le16(entry + FAT32_DE_CRT_TIME, dos_time);
    write_le16(entry + FAT32_DE_CRT_DATE, dos_date);
    write_le16(entry + FAT32_DE_WRT_TIME, dos_time);
    write_le16(entry + FAT32_DE_WRT_DATE, dos_date);
    write_le16(entry + FAT32_DE_LST_ACC_DATE, dos_date);
    write_checked(mnt, entry, FAT32_DE_SIZE, byte_off + FAT32_DE_SIZE);
    r = create_dir_entry(mnt, parent_cluster, leaf, new_cluster, 0, FAT32_ATTR_DIRECTORY);
    if (r < 0) {
        free_chain(mnt, new_cluster);
        spin_unlock_irqrestore(&vfat_lock, irq);
        return r;
    }
    spin_unlock_irqrestore(&vfat_lock, irq);
    return 0;
}

static int erase_dir_entries(const vfat_mount_t *mnt, uint32_t dir_cluster, uint64_t offset, uint32_t lfn_count) {
    for (uint32_t i = 0; i < lfn_count + 1; i++) {
        uint64_t cur = offset + i * FAT32_DE_SIZE;
        uint8_t e[FAT32_DE_SIZE];
        int r = read_dir_entry_at(mnt, dir_cluster, cur, e);
        if (r < 0) return r;
        e[0] = 0xE5;
        r = write_dir_entry_at(mnt, dir_cluster, cur, e);
        if (r < 0) return r;
    }
    return 0;
}

static int count_lfn_for_entry(const vfat_mount_t *mnt, uint32_t dir_cluster, uint64_t sfn_offset, uint32_t *out_lfn) {
    if (sfn_offset < FAT32_DE_SIZE) { *out_lfn = 0; return 0; }
    uint32_t cnt = 0;
    uint64_t cur = sfn_offset - FAT32_DE_SIZE;
    while (1) {
        uint8_t e[FAT32_DE_SIZE];
        int r = read_dir_entry_at(mnt, dir_cluster, cur, e);
        if (r < 0) break;
        if (e[FAT32_DE_ATTR] != FAT32_ATTR_LFN) break;
        cnt++;
        if (e[FAT32_LFN_ORDER] & 0x40) break;
        if (cur == 0) break;
        cur -= FAT32_DE_SIZE;
        if (cnt > VFAT_MAX_LFN_ENTRIES) break;
    }
    *out_lfn = cnt;
    return 0;
}

int unlink_vfat(const char *path) {
    if (!path) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    if (relative[0] == '\0') return -EISDIR;
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    uint32_t parent_cluster = 0;
    char leaf[VFAT_MAX_COMPONENT + 1];
    int r = resolve_parent(mnt, relative, &parent_cluster, leaf);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint64_t sfn_offset = 0;
    vfat_dirent_t de;
    r = locate_entry(mnt, parent_cluster, leaf, strlen(leaf), &sfn_offset, &de);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    if (de.attr & FAT32_ATTR_DIRECTORY) { spin_unlock_irqrestore(&vfat_lock, irq); return -EISDIR; }
    uint32_t lfn = 0;
    count_lfn_for_entry(mnt, parent_cluster, sfn_offset, &lfn);
    uint64_t first_lfn = lfn ? sfn_offset - lfn * FAT32_DE_SIZE : sfn_offset;
    if (de.first_cluster >= 2) free_chain(mnt, de.first_cluster);
    r = erase_dir_entries(mnt, parent_cluster, first_lfn, lfn);
    spin_unlock_irqrestore(&vfat_lock, irq);
    return r;
}

int rmdir_vfat(const char *path) {
    if (!path) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    if (relative[0] == '\0') return -EBUSY;
    uint64_t irq;
    spin_lock_irqsave(&vfat_lock, &irq);
    uint32_t parent_cluster = 0;
    char leaf[VFAT_MAX_COMPONENT + 1];
    int r = resolve_parent(mnt, relative, &parent_cluster, leaf);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    uint64_t sfn_offset = 0;
    vfat_dirent_t de;
    r = locate_entry(mnt, parent_cluster, leaf, strlen(leaf), &sfn_offset, &de);
    if (r < 0) { spin_unlock_irqrestore(&vfat_lock, irq); return r; }
    if (!(de.attr & FAT32_ATTR_DIRECTORY)) { spin_unlock_irqrestore(&vfat_lock, irq); return -ENOTDIR; }
    uint32_t dir_cluster = de.first_cluster;
    bool empty = true;
    if (dir_cluster >= 2) {
        uint32_t cur = dir_cluster;
        while (cur >= 2 && cur < FAT32_CHAIN_EOF && empty) {
            uint8_t *buf = malloc(mnt->cluster_size);
            if (!buf) { spin_unlock_irqrestore(&vfat_lock, irq); return -ENOMEM; }
            read_cluster_data(mnt, cur, buf, mnt->cluster_size, 0);
            uint32_t entries = mnt->cluster_size / FAT32_DE_SIZE;
            for (uint32_t i = 0; i < entries; i++) {
                uint8_t *e = buf + i * FAT32_DE_SIZE;
                if (e[0] == 0x00) break;
                if (e[0] == 0xE5) continue;
                if (e[FAT32_DE_ATTR] == FAT32_ATTR_LFN) continue;
                if (e[FAT32_DE_ATTR] == FAT32_ATTR_VOLUME_ID) continue;
                if (e[0] == '.' && e[1] == ' ' ) continue;
                if (e[0] == '.' && e[1] == '.' && e[2] == ' ') continue;
                bool is_dot = e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '));
                if (is_dot) continue;
                empty = false;
                break;
            }
            free(buf);
            cur = get_next_cluster(mnt, cur);
        }
    } else if (mnt->fat_bits == 16 && dir_cluster == 0) empty = false;
    if (!empty) { spin_unlock_irqrestore(&vfat_lock, irq); return -ENOTEMPTY; }
    if (dir_cluster >= 2) free_chain(mnt, dir_cluster);
    uint32_t lfn = 0;
    count_lfn_for_entry(mnt, parent_cluster, sfn_offset, &lfn);
    uint64_t first_lfn = lfn ? sfn_offset - lfn * FAT32_DE_SIZE : sfn_offset;
    r = erase_dir_entries(mnt, parent_cluster, first_lfn, lfn);
    spin_unlock_irqrestore(&vfat_lock, irq);
    return r;
}

static int select_child(const vfat_dirent_t *dirent, void *context) {
    vfat_readdir_context_t *ctx = context;
    const char *name = dirent->long_name_len > 0 ? dirent->long_name : dirent->short_name;
    size_t len = dirent->long_name_len > 0 ? dirent->long_name_len : dirent->short_name_len;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return 0;
    if (ctx->seen++ != ctx->wanted) return 0;
    if (len + 1 > ctx->name_size) return -ENAMETOOLONG;
    memcpy(ctx->name, name, len);
    ctx->name[len] = '\0';
    if (dirent->attr & FAT32_ATTR_DIRECTORY) *ctx->type = DT_DIR;
    else *ctx->type = DT_REG;
    *ctx->ino = dirent->first_cluster;
    return 1;
}

int get_next_vfat_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino) {
    if (!index || *index < 0 || !path || !name || !name_size || !type || !ino) return -EINVAL;
    const char *relative;
    vfat_mount_t *mnt = find_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t cluster;
    vfat_dirent_t dirent;
    int status = resolve_path(mnt, relative, true, &cluster, &dirent);
    if (status < 0) return status;
    if (!(dirent.attr & FAT32_ATTR_DIRECTORY)) return -ENOTDIR;
    vfat_readdir_context_t ctx = { *index, 0, name, name_size, type, ino };
    status = walk_directory(mnt, cluster, select_child, &ctx);
    if (status == 1) { (*index)++; return 0; }
    return status < 0 ? status : -ENOENT;
}
