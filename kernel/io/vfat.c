#include <stdbool.h>
#include <errno.h>
#include <main/string.h>
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

static int64_t read_cluster_data(const vfat_mount_t *mnt, uint32_t cluster, void *buf, uint64_t count, uint64_t offset) {
    if (cluster < 2 || cluster >= mnt->total_clusters + 2) return -EIO;
    if (offset >= mnt->cluster_size) return 0;
    if (count > mnt->cluster_size - offset) count = mnt->cluster_size - offset;
    uint64_t cluster_sector = mnt->data_offset / mnt->bytes_per_sector + (uint64_t)(cluster - 2) * mnt->sectors_per_cluster;
    uint64_t byte_off = cluster_sector * mnt->bytes_per_sector + offset;
    return read_checked(mnt, buf, count, byte_off) == 0 ? (int64_t)count : -EIO;
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
            if (read_device(dev, boot, 512, off) != 512) continue;
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
    st->st_ino = dirent.first_cluster;
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
    if (offset >= dirent.file_size) return 0;
    return read_chain(mnt, dirent.first_cluster, buffer, count, offset, dirent.file_size);
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
