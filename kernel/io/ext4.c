#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/statx.h>
#include <sys/mount.h>
#include <main/string.h>
#include <main/timekeeping.h>
#include <io/devices.h>
#include <io/ext4.h>
#include <mm/mm.h>

ext4_mount_t ext4_mounts[EXT4_MAX_MOUNTS];
spinlock_t ext4_lock = SPINLOCK_INIT;

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
    p[2] = (value >> 16) & 0xFF;
    p[3] = (value >> 24) & 0xFF;
}

static uint64_t combine_u32s(uint32_t lo, uint32_t hi) {
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static uint32_t calculate_crc32c(uint32_t crc, const void *buffer, size_t size) {
    const uint8_t *bytes = buffer;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (EXT4_CRC32C_POLY & mask);
        }
    }
    return crc;
}

static uint16_t calculate_crc16(uint16_t crc, const void *buffer, size_t size) {
    const uint8_t *bytes = buffer;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (uint16_t)((crc >> 1) ^ (EXT4_CRC16_POLY & mask));
        }
    }
    return crc;
}

static uint32_t calculate_ext4_seed(const ext4_mount_t *mnt) {
    if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) return mnt->checksum_seed;
    return calculate_crc32c(UINT32_MAX, mnt->uuid, sizeof(mnt->uuid));
}

static int validate_superblock_checksum(const ext4_mount_t *mnt, const uint8_t super[EXT4_SUPER_SIZE]) {
    if (!(mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return 0;
    uint32_t stored = read_le32(super + 0x3FC);
    uint32_t calculated = calculate_crc32c(UINT32_MAX, super, 0x3FC);
    return stored == calculated ? 0 : -EIO;
}

static void update_superblock_checksum(ext4_mount_t *mnt, uint8_t super[EXT4_SUPER_SIZE]) {
    if (!(mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return;
    write_le32(super + 0x3FC, 0);
    write_le32(super + 0x3FC, calculate_crc32c(UINT32_MAX, super, 0x3FC));
}

static uint16_t calculate_group_checksum(const ext4_mount_t *mnt, uint32_t group, const uint8_t descriptor[64]) {
    uint8_t copy[64];
    memcpy(copy, descriptor, mnt->desc_size);
    write_le16(copy + 0x1E, 0);
    uint8_t group_bytes[4];
    write_le32(group_bytes, group);
    if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t crc = calculate_ext4_seed(mnt);
        crc = calculate_crc32c(crc, group_bytes, sizeof(group_bytes));
        crc = calculate_crc32c(crc, copy, 0x1E);
        crc = calculate_crc32c(crc, copy + 0x1E, 2);
        if (mnt->desc_size > 32) crc = calculate_crc32c(crc, copy + 32, mnt->desc_size - 32);
        return (uint16_t)crc;
    }
    if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) {
        uint16_t crc = calculate_crc16(UINT16_MAX, mnt->uuid, sizeof(mnt->uuid));
        crc = calculate_crc16(crc, group_bytes, sizeof(group_bytes));
        crc = calculate_crc16(crc, copy, 0x1E);
        if (mnt->desc_size > 32) crc = calculate_crc16(crc, copy + 32, mnt->desc_size - 32);
        return crc;
    }
    return 0;
}

static int validate_group_checksum(const ext4_mount_t *mnt, uint32_t group, const uint8_t descriptor[64]) {
    bool metadata_csum = mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    bool legacy_csum = mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM;
    if (!metadata_csum && !legacy_csum) return 0;
    return read_le16(descriptor + 0x1E) == calculate_group_checksum(mnt, group, descriptor) ? 0 : -EIO;
}

static void update_group_checksum(ext4_mount_t *mnt, uint32_t group, uint8_t descriptor[64]) {
    write_le16(descriptor + 0x1E, 0);
    uint16_t checksum = calculate_group_checksum(mnt, group, descriptor);
    if (checksum || (mnt->feature_ro_compat & (EXT4_FEATURE_RO_COMPAT_METADATA_CSUM | EXT4_FEATURE_RO_COMPAT_GDT_CSUM))) {
        write_le16(descriptor + 0x1E, checksum);
    }
}

static bool check_ext4_high_checksum(const ext4_mount_t *mnt, const uint8_t raw[EXT4_MAX_BLOCK_SIZE]) {
    if (mnt->inode_size <= EXT4_GOOD_OLD_INODE_SIZE || mnt->inode_size < 132) return false;
    uint16_t extra_size = read_le16(raw + 128);
    return extra_size >= 4 && mnt->inode_size >= 132;
}

static uint32_t calculate_inode_checksum(const ext4_mount_t *mnt, uint32_t ino, const uint8_t raw[EXT4_MAX_BLOCK_SIZE]) {
    uint8_t ino_bytes[4];
    uint8_t generation[4];
    uint8_t zero[2] = {0, 0};
    write_le32(ino_bytes, ino);
    write_le32(generation, read_le32(raw + 0x64));
    uint32_t crc = calculate_ext4_seed(mnt);
    crc = calculate_crc32c(crc, ino_bytes, sizeof(ino_bytes));
    crc = calculate_crc32c(crc, generation, sizeof(generation));
    crc = calculate_crc32c(crc, raw, 0x7C);
    crc = calculate_crc32c(crc, zero, sizeof(zero));
    size_t tail = mnt->inode_size < 0x82 ? mnt->inode_size : 0x82;
    crc = calculate_crc32c(crc, raw + 0x7E, tail - 0x7E);
    if (check_ext4_high_checksum(mnt, raw)) {
        crc = calculate_crc32c(crc, zero, sizeof(zero));
        if (mnt->inode_size > 0x84) crc = calculate_crc32c(crc, raw + 0x84, mnt->inode_size - 0x84);
    } else if (mnt->inode_size > 0x82) {
        crc = calculate_crc32c(crc, raw + 0x82, mnt->inode_size - 0x82);
    }
    return crc;
}

static int validate_inode_checksum(const ext4_mount_t *mnt, uint32_t ino, const uint8_t raw[EXT4_MAX_BLOCK_SIZE]) {
    if (!(mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return 0;
    uint32_t stored = read_le16(raw + 0x7C);
    uint32_t calculated = calculate_inode_checksum(mnt, ino, raw);
    if (check_ext4_high_checksum(mnt, raw)) stored |= (uint32_t)read_le16(raw + 0x82) << 16;
    else calculated &= 0xFFFF;
    return stored == calculated ? 0 : -EIO;
}

static void update_inode_checksum(ext4_mount_t *mnt, uint32_t ino, uint8_t raw[EXT4_MAX_BLOCK_SIZE]) {
    if (!(mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return;
    write_le16(raw + 0x7C, 0);
    if (check_ext4_high_checksum(mnt, raw)) write_le16(raw + 0x82, 0);
    uint32_t checksum = calculate_inode_checksum(mnt, ino, raw);
    write_le16(raw + 0x7C, checksum);
    if (check_ext4_high_checksum(mnt, raw)) write_le16(raw + 0x82, checksum >> 16);
}

static void finalize_group_states(ext4_mount_t *mnt, const ext4_write_context_t *context);
static int commit_ext4_group_states(ext4_mount_t *mnt, const ext4_write_context_t *context);
static int truncate_ext4_extents(ext4_mount_t *mnt, uint8_t *raw, uint32_t cutoff, ext4_write_context_t *context, uint32_t *freed);
static int free_ext4_legacy_tree(ext4_mount_t *mnt, uint8_t *raw, ext4_write_context_t *context, uint32_t *freed);
static int remove_ext4_dirent(ext4_mount_t *mnt, const uint8_t *parent_raw, const char *name, size_t name_length, uint64_t *dir_block, uint8_t *dir_data);
static int free_ext4_inode_bit(ext4_mount_t *mnt, uint8_t super[EXT4_SUPER_SIZE], uint32_t ino, uint8_t desc[64], uint8_t **bitmap, uint64_t *bitmap_block, uint32_t *group);
static int stage_superblock(ext4_mount_t *mnt, ext4_journal_batch_t *batch, const uint8_t super[EXT4_SUPER_SIZE]);
static int stage_full_block(ext4_mount_t *mnt, ext4_journal_batch_t *batch, uint64_t fs_block, const void *data);
static int stage_group_descriptor(ext4_mount_t *mnt, ext4_journal_batch_t *batch, uint32_t group, const uint8_t desc[64]);
static int stage_inode_bytes(ext4_mount_t *mnt, ext4_journal_batch_t *batch, const ext4_inode_location_t *location, const uint8_t *raw, uint16_t size);
static int commit_staged_blocks(ext4_mount_t *mnt, ext4_journal_batch_t *batch);
static void free_staged_batch(ext4_journal_batch_t *batch);
static bool merge_ext4_inode_desc(const ext4_mount_t *mnt, ext4_write_context_t *context, uint32_t inode_group, const uint8_t desc[64]);

static int read_checked_device(const ext4_mount_t *mnt, void *buf, uint64_t count, uint64_t offset) {
    if (!mnt || !buf) return -EINVAL;
    if (offset > mnt->device_size || count > mnt->device_size - offset) return -EIO;
    uint64_t got = read_device(mnt->device, buf, count, offset, 0);
    if ((int64_t)got < 0) return (int)(int64_t)got;
    return got == count ? 0 : -EIO;
}

static int write_checked_device(const ext4_mount_t *mnt, const void *buf, uint64_t count, uint64_t offset) {
    if (!mnt || !buf) return -EINVAL;
    if (offset > mnt->device_size || count > mnt->device_size - offset) return -EIO;
    uint64_t wrote = write_device(mnt->device, buf, count, offset, 0);
    if ((int64_t)wrote < 0) return (int)(int64_t)wrote;
    return wrote == count ? 0 : -EIO;
}

static int read_ext4_block(const ext4_mount_t *mnt, uint64_t block, void *buf) {
    if (block >= mnt->blocks_count) return -EIO;
    if (block > UINT64_MAX / mnt->block_size) return -EOVERFLOW;
    return read_checked_device(mnt, buf, mnt->block_size, block * (uint64_t)mnt->block_size);
}

static int write_ext4_block(const ext4_mount_t *mnt, uint64_t block, const void *buf) {
    if (block >= mnt->blocks_count) return -EIO;
    if (block > UINT64_MAX / mnt->block_size) return -EOVERFLOW;
    return write_checked_device(mnt, buf, mnt->block_size, block * (uint64_t)mnt->block_size);
}

static const char *get_device_name(const char *source) {
    if (!source) return NULL;
    while (*source == '/') source++;
    if (strncmp(source, "dev/", 4) == 0) source += 4;
    return source;
}

static bool check_path_under(const char *path, const char *target, const char **relative) {
    size_t length = strlen(target);
    if (length == 1 && target[0] == '/') {
        if (path[0] != '/') return false;
        const char *root_relative = path + 1;
        if (relative) *relative = root_relative;
        return true;
    }
    if (strncmp(path, target, length) != 0) return false;
    if (path[length] != '\0' && path[length] != '/') return false;
    const char *path_relative = path + length;
    while (*path_relative == '/') path_relative++;
    if (relative) *relative = path_relative;
    return true;
}

static ext4_mount_t *find_ext4_mount(const char *path, const char **relative) {
    ext4_mount_t *best = NULL;
    const char *best_rel = NULL;
    size_t best_len = 0;
    for (int i = 0; i < EXT4_MAX_MOUNTS; i++) {
        const char *rel;
        if (!ext4_mounts[i].active || !check_path_under(path, ext4_mounts[i].target, &rel)) continue;
        size_t len = strlen(ext4_mounts[i].target);
        if (!best || len > best_len) {
            best = &ext4_mounts[i];
            best_rel = rel;
            best_len = len;
        }
    }
    if (relative) *relative = best_rel;
    return best;
}

static int read_ext4_group_desc(const ext4_mount_t *mnt, uint32_t group, uint8_t desc[64]) {
    if (group >= mnt->groups_count) return -EIO;
    uint64_t delta = (uint64_t)group * mnt->desc_size;
    if (delta > UINT64_MAX - mnt->gdt_offset) return -EOVERFLOW;
    memset(desc, 0, 64);
    return read_checked_device(mnt, desc, mnt->desc_size, mnt->gdt_offset + delta);
}

static int read_ext4_inode(const ext4_mount_t *mnt, uint32_t ino, ext4_inode_t *inode) {
    if (!ino || ino > mnt->inodes_count || !inode) return -ENOENT;
    uint32_t group = (ino - 1) / mnt->inodes_per_group;
    uint32_t index = (ino - 1) % mnt->inodes_per_group;
    uint8_t desc[64];
    int status = read_ext4_group_desc(mnt, group, desc);
    if (status < 0) return status;

    uint64_t table = read_le32(desc + 8);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) table |= (uint64_t)read_le32(desc + 40) << 32;
    if (!table || table >= mnt->blocks_count) return -EIO;

    uint64_t byte_off = (uint64_t)index * mnt->inode_size;
    if (table > UINT64_MAX / mnt->block_size) return -EOVERFLOW;
    uint64_t offset = table * (uint64_t)mnt->block_size;
    if (byte_off > UINT64_MAX - offset) return -EOVERFLOW;

    uint8_t raw[160];
    memset(raw, 0, sizeof(raw));
    size_t raw_size = mnt->inode_size < sizeof(raw) ? mnt->inode_size : sizeof(raw);
    status = read_checked_device(mnt, raw, raw_size, offset + byte_off);
    if (status < 0) return status;

    memset(inode, 0, sizeof(*inode));
    inode->mode = read_le16(raw + 0);
    inode->uid = read_le16(raw + 2) | ((uint32_t)read_le16(raw + 120) << 16);
    inode->gid = read_le16(raw + 24) | ((uint32_t)read_le16(raw + 122) << 16);
    inode->size = read_le32(raw + 4);
    if ((inode->mode & S_IFMT) == S_IFREG) inode->size |= (uint64_t)read_le32(raw + 108) << 32;
    inode->atime = read_le32(raw + 8);
    inode->ctime = read_le32(raw + 12);
    inode->mtime = read_le32(raw + 16);
    inode->deleted = read_le32(raw + 20);
    inode->links = read_le16(raw + 26);
    inode->blocks_512 = read_le32(raw + 28);
    inode->flags = read_le32(raw + 32);
    inode->generation = read_le32(raw + 100);
    if (raw_size >= 130 && mnt->inode_size > EXT4_GOOD_OLD_INODE_SIZE) inode->extra_size = read_le16(raw + 128);
    if (raw_size >= 152 && inode->extra_size >= 24) {
        uint32_t extra = read_le32(raw + 148);
        inode->btime.tv_sec = (int64_t)(int32_t)read_le32(raw + 144) + ((int64_t)(extra & 3U) << 32);
        inode->btime.tv_nsec = extra >> 2;
        inode->has_btime = inode->btime.tv_nsec < 1000000000L;
    }
    if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) {
        inode->blocks_512 |= (uint64_t)read_le16(raw + 116) << 32;
        if (inode->flags & EXT4_HUGE_FILE_FL) inode->blocks_512 *= (mnt->block_size / 512U);
    }
    memcpy(inode->block, raw + 40, sizeof(inode->block));
    return inode->mode ? 0 : -ENOENT;
}

static int map_ext4_extent_block(const ext4_mount_t *mnt, const uint8_t *root, size_t root_size, uint32_t logical, uint16_t expected_depth, uint64_t *physical) {
    if (root_size < 12 || read_le16(root) != EXT4_EXTENT_MAGIC) return -EIO;
    uint16_t entries = read_le16(root + 2);
    uint16_t maximum = read_le16(root + 4);
    uint16_t depth = read_le16(root + 6);
    if (depth != expected_depth || depth > 5 || entries > maximum) return -EIO;
    if ((uint64_t)maximum * 12U + 12U > root_size) return -EIO;

    if (depth == 0) {
        for (uint16_t i = 0; i < entries; i++) {
            const uint8_t *ex = root + 12U + (size_t)i * 12U;
            uint32_t first = read_le32(ex);
            uint16_t raw_len = read_le16(ex + 4);
            bool unwritten = raw_len > 32768U;
            uint32_t len = unwritten ? (uint32_t)raw_len - 32768U : raw_len;
            if (!len || logical < first || logical - first >= len) continue;
            uint64_t start = (uint64_t)read_le32(ex + 8) |
                             ((uint64_t)read_le16(ex + 6) << 32);
            if (start >= mnt->blocks_count || logical - first >= mnt->blocks_count - start) return -EIO;
            *physical = start + (logical - first);
            return unwritten ? 2 : 1;
        }
        *physical = 0;
        return 0;
    }

    const uint8_t *chosen = NULL;
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *idx = root + 12U + (size_t)i * 12U;
        uint32_t first = read_le32(idx);
        if (first > logical) break;
        chosen = idx;
    }
    if (!chosen) {
        *physical = 0;
        return 0;
    }
    uint64_t child = (uint64_t)read_le32(chosen + 4) |
                     ((uint64_t)read_le16(chosen + 8) << 32);
    if (!child || child >= mnt->blocks_count) return -EIO;
    uint8_t *block = malloc(mnt->block_size);
    if (!block) return -ENOMEM;
    int status = read_ext4_block(mnt, child, block);
    if (status == 0) status = map_ext4_extent_block(mnt, block, mnt->block_size, logical, depth - 1, physical);
    free(block);
    return status;
}

static int read_ext4_indirect_ptr(const ext4_mount_t *mnt, uint64_t block, uint32_t index, uint32_t *value) {
    if (!block) {
        *value = 0;
        return 0;
    }
    if (block >= mnt->blocks_count || index >= mnt->block_size / 4U) return -EIO;
    uint8_t raw[4];
    int status = read_checked_device(mnt, raw, sizeof(raw),
                                     block * (uint64_t)mnt->block_size + (uint64_t)index * 4U);
    if (status < 0) return status;
    *value = read_le32(raw);
    if (*value >= mnt->blocks_count) return -EIO;
    return 0;
}

static int map_ext4_legacy_block(const ext4_mount_t *mnt, const ext4_inode_t *inode, uint32_t logical, uint64_t *physical) {
    uint32_t ptrs = mnt->block_size / 4U;
    if (logical < 12U) {
        *physical = read_le32(inode->block + logical * 4U);
        return *physical < mnt->blocks_count ? (*physical != 0) : -EIO;
    }
    logical -= 12U;
    uint32_t block = 0;
    int status;
    if (logical < ptrs) {
        status = read_ext4_indirect_ptr(mnt, read_le32(inode->block + 48), logical, &block);
    } else {
        logical -= ptrs;
        uint64_t square = (uint64_t)ptrs * ptrs;
        if ((uint64_t)logical < square) {
            uint32_t first;
            status = read_ext4_indirect_ptr(mnt, read_le32(inode->block + 52),
                                            logical / ptrs, &first);
            if (status == 0) status = read_ext4_indirect_ptr(mnt, first, logical % ptrs, &block);
        } else {
            uint64_t remain = (uint64_t)logical - square;
            uint64_t cube = square * ptrs;
            if (remain >= cube) return -EFBIG;
            uint32_t first, second;
            status = read_ext4_indirect_ptr(mnt, read_le32(inode->block + 56),
                                            (uint32_t)(remain / square), &first);
            remain %= square;
            if (status == 0) status = read_ext4_indirect_ptr(mnt, first, (uint32_t)(remain / ptrs), &second);
            if (status == 0) status = read_ext4_indirect_ptr(mnt, second, (uint32_t)(remain % ptrs), &block);
        }
    }
    if (status < 0) return status;
    *physical = block;
    return block ? 1 : 0;
}

static int map_ext4_inode_block(const ext4_mount_t *mnt, const ext4_inode_t *inode, uint32_t logical, uint64_t *physical) {
    if (inode->flags & EXT4_EXTENTS_FL) {
        uint16_t depth = read_le16(inode->block + 6);
        return map_ext4_extent_block(mnt, inode->block, sizeof(inode->block), logical, depth, physical);
    }
    return map_ext4_legacy_block(mnt, inode, logical, physical);
}

static int64_t read_ext4_inode_data(const ext4_mount_t *mnt, const ext4_inode_t *inode, void *buffer, uint64_t count, uint64_t offset) {
    if (offset >= inode->size || count == 0) return 0;
    if (count > inode->size - offset) count = inode->size - offset;
    uint8_t *out = buffer;
    uint64_t done = 0;
    while (done < count) {
        uint64_t absolute = offset + done;
        uint64_t logical64 = absolute / mnt->block_size;
        if (logical64 > UINT32_MAX) return done ? (int64_t)done : -EFBIG;
        uint32_t in_block = absolute % mnt->block_size;
        uint64_t chunk = mnt->block_size - in_block;
        if (chunk > count - done) chunk = count - done;
        uint64_t physical;
        int mapped = map_ext4_inode_block(mnt, inode, (uint32_t)logical64, &physical);
        if (mapped < 0) return done ? (int64_t)done : mapped;
        if (mapped != 1) {
            memset(out + done, 0, chunk);
        } else {
            uint64_t disk_off = physical * (uint64_t)mnt->block_size + in_block;
            int status = read_checked_device(mnt, out + done, chunk, disk_off);
            if (status < 0) return done ? (int64_t)done : status;
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int walk_ext4_directory(const ext4_mount_t *mnt, const ext4_inode_t *dir, ext4_dir_callback_t callback, void *context) {
    if ((dir->mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
    uint8_t *block = malloc(mnt->block_size);
    if (!block) return -ENOMEM;
    uint64_t offset = 0;
    int result = 0;
    while (offset < dir->size) {
        uint64_t remaining = dir->size - offset;
        uint64_t amount = remaining < mnt->block_size ? remaining : mnt->block_size;
        int64_t got = read_ext4_inode_data(mnt, dir, block, amount, offset);
        if (got < 0) {
            result = (int)got;
            break;
        }
        if ((uint64_t)got != amount) {
            result = -EIO;
            break;
        }
        uint32_t pos = 0;
        while (pos < amount) {
            if (amount - pos < 8) {
                result = -EIO;
                goto out;
            }
            uint32_t ino = read_le32(block + pos);
            uint16_t rec_len = read_le16(block + pos + 4);
            uint8_t name_len = block[pos + 6];
            uint8_t type = (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE)
                               ? block[pos + 7]
                               : DT_UNKNOWN;
            if (!rec_len && !ino) break;
            if (rec_len < 8 || (rec_len & 3) || rec_len > amount - pos || name_len > rec_len - 8) {
                result = -EIO;
                goto out;
            }
            if (ino && name_len) {
                result = callback(ino, type, (char *)block + pos + 8,
                                  name_len, context);
                if (result != 0) goto out;
            }
            pos += rec_len;
        }
        offset += amount;
    }
out:
    free(block);
    return result;
}

static int match_ext4_lookup(uint32_t ino, uint8_t type, const char *name, uint8_t length, void *opaque) {
    ext4_lookup_context_t *ctx = opaque;
    if (ctx->length == length && memcmp(ctx->name, name, length) == 0) {
        ctx->ino = ino;
        ctx->type = type;
        return 1;
    }
    return 0;
}

static int lookup_ext4_child(const ext4_mount_t *mnt, uint32_t dir_ino, const char *name, size_t length, uint32_t *child) {
    if (!length || length > 255) return -ENAMETOOLONG;
    ext4_inode_t dir;
    int status = read_ext4_inode(mnt, dir_ino, &dir);
    if (status < 0) return status;
    ext4_lookup_context_t ctx = {name, length, 0, 0};
    status = walk_ext4_directory(mnt, &dir, match_ext4_lookup, &ctx);
    if (status == 1) {
        *child = ctx.ino;
        return 0;
    }
    return status < 0 ? status : -ENOENT;
}

static int read_ext4_symlink_inode(const ext4_mount_t *mnt, const ext4_inode_t *inode, char *out, size_t out_size) {
    if ((inode->mode & S_IFMT) != S_IFLNK) return -EINVAL;
    if (!out_size) return -ENAMETOOLONG;
    if (inode->size >= out_size || inode->size >= EXT4_MAX_PATH) return -ENAMETOOLONG;
    if (inode->size <= sizeof(inode->block) && !(inode->flags & EXT4_EXTENTS_FL)) {
        memcpy(out, inode->block, inode->size);
    } else {
        int64_t got = read_ext4_inode_data(mnt, inode, out, inode->size, 0);
        if (got < 0) return (int)got;
        if ((uint64_t)got != inode->size) return -EIO;
    }
    out[inode->size] = '\0';
    return (int)inode->size;
}

static int splice_ext4_symlink(char work[EXT4_MAX_PATH], size_t component_start, size_t component_end, const char *target) {
    char next[EXT4_MAX_PATH];
    size_t target_len = strlen(target);
    size_t suffix_len = strlen(work + component_end);
    size_t prefix_len = target[0] == '/' ? 0 : component_start;
    if (prefix_len + target_len + suffix_len + 1 > sizeof(next)) return -ENAMETOOLONG;
    if (prefix_len) memcpy(next, work, prefix_len);
    memcpy(next + prefix_len, target, target_len);
    memcpy(next + prefix_len + target_len, work + component_end,
           suffix_len + 1);
    strlcpy(work, next, EXT4_MAX_PATH);
    return 0;
}

static int resolve_ext4_inode(const ext4_mount_t *mnt, const char *relative, bool follow_final, uint32_t *resolved, ext4_inode_t *resolved_inode) {
    char work[EXT4_MAX_PATH];
    if (strlen(relative) + 2 > sizeof(work)) return -ENAMETOOLONG;
    work[0] = '/';
    strlcpy(work + 1, relative, sizeof(work) - 1);

    int symlinks = 0;
restart: {
    uint32_t current = EXT4_ROOT_INO;
    size_t pos = 0;
    while (work[pos]) {
        while (work[pos] == '/')
            pos++;
        if (!work[pos]) break;
        size_t start = pos;
        while (work[pos] && work[pos] != '/')
            pos++;
        size_t end = pos;
        bool final = true;
        for (size_t p = end; work[p]; p++)
            if (work[p] != '/') {
                final = false;
                break;
            }

        size_t len = end - start;
        if (len == 1 && work[start] == '.') continue;
        uint32_t child;
        int status = lookup_ext4_child(mnt, current, work + start, len, &child);
        if (status < 0) return status;
        ext4_inode_t inode;
        status = read_ext4_inode(mnt, child, &inode);
        if (status < 0) return status;
        if ((inode.mode & S_IFMT) == S_IFLNK && (follow_final || !final)) {
            if (++symlinks > EXT4_MAX_SYMLINKS) return -ELOOP;
            char target[EXT4_MAX_PATH];
            status = read_ext4_symlink_inode(mnt, &inode, target, sizeof(target));
            if (status < 0) return status;
            status = splice_ext4_symlink(work, start, end, target);
            if (status < 0) return status;
            goto restart;
        }
        if (!final && (inode.mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
        current = child;
    }
    int status = read_ext4_inode(mnt, current, resolved_inode);
    if (status < 0) return status;
    *resolved = current;
    return 0;
}
}

static int write_ext4_group_desc(const ext4_mount_t *mnt, uint32_t group, const uint8_t desc[64]) {
    if (group >= mnt->groups_count) return -EIO;
    uint64_t delta = (uint64_t)group * mnt->desc_size;
    if (delta > UINT64_MAX - mnt->gdt_offset) return -EOVERFLOW;
    return write_checked_device(mnt, desc, mnt->desc_size, mnt->gdt_offset + delta);
}

static int locate_ext4_inode(const ext4_mount_t *mnt, uint32_t ino, ext4_inode_location_t *location) {
    if (!ino || ino > mnt->inodes_count || !location) return -ENOENT;
    uint32_t group = (ino - 1) / mnt->inodes_per_group;
    uint32_t index = (ino - 1) % mnt->inodes_per_group;
    uint8_t desc[64];
    int status = read_ext4_group_desc(mnt, group, desc);
    if (status < 0) return status;
    uint64_t table = read_le32(desc + 8);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) table |= (uint64_t)read_le32(desc + 40) << 32;
    if (!table || table >= mnt->blocks_count) return -EIO;
    uint64_t byte_offset = (uint64_t)index * mnt->inode_size;
    if (table > UINT64_MAX / mnt->block_size) return -EOVERFLOW;
    uint64_t offset = table * (uint64_t)mnt->block_size;
    if (byte_offset > UINT64_MAX - offset) return -EOVERFLOW;
    location->byte_offset = offset + byte_offset;
    location->group = group;
    location->block_count = (mnt->inode_size + mnt->block_size - 1) / mnt->block_size;
    return 0;
}

static int read_ext4_inode_bytes(const ext4_mount_t *mnt, uint32_t ino, uint8_t *raw) {
    if (!raw) return -EINVAL;
    ext4_inode_location_t location;
    int status = locate_ext4_inode(mnt, ino, &location);
    if (status < 0) return status;
    status = read_checked_device(mnt, raw, mnt->inode_size, location.byte_offset);
    if (status < 0) return status;
    return validate_inode_checksum(mnt, ino, raw);
}

static int write_ext4_inode_bytes(const ext4_mount_t *mnt, const ext4_inode_location_t *location, const uint8_t *raw) {
    if (!location || !raw) return -EINVAL;
    return write_checked_device(mnt, raw, mnt->inode_size, location->byte_offset);
}

static bool check_bitmap_bit(const uint8_t *bitmap, uint32_t bit) {
    return bitmap[bit >> 3] & (1U << (bit & 7U));
}

static void set_bitmap_bit(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit >> 3] |= 1U << (bit & 7U);
}

static int load_ext4_group(const ext4_mount_t *mnt, ext4_write_context_t *context, uint32_t group) {
    for (size_t i = 0; i < context->group_count; i++) if (context->groups[i].group == group) return 0;
    uint8_t desc[64];
    int status = read_ext4_group_desc(mnt, group, desc);
    if (status < 0) return status;
    status = validate_group_checksum(mnt, group, desc);
    if (status < 0) return status;
    if (read_le16(desc + 0x12) & EXT4_BG_BLOCK_UNINIT) return -EOPNOTSUPP;
    uint64_t bitmap_block = read_le32(desc);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) bitmap_block |= (uint64_t)read_le32(desc + 0x20) << 32;
    if (!bitmap_block || bitmap_block >= mnt->blocks_count) return -EIO;
    uint8_t *bitmap = malloc(mnt->block_size);
    if (!bitmap) return -ENOMEM;
    status = read_ext4_block(mnt, bitmap_block, bitmap);
    if (status < 0) {
        free(bitmap);
        return status;
    }
    uint32_t bitmap_bytes = mnt->blocks_per_group >> 3;
    if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), bitmap, bitmap_bytes);
        uint32_t stored = read_le16(desc + 0x18);
        if (mnt->desc_size >= 64) stored |= (uint32_t)read_le16(desc + 0x38) << 16;
        else checksum &= 0xFFFF;
        if (stored != checksum) {
            free(bitmap);
            return -EIO;
        }
    }
    uint64_t group_start = mnt->first_data_block + (uint64_t)group * mnt->blocks_per_group;
    if (group_start >= mnt->blocks_count) {
        free(bitmap);
        return -EIO;
    }
    uint64_t available = mnt->blocks_count - group_start;
    uint32_t valid_blocks = available < mnt->blocks_per_group ? (uint32_t)available : mnt->blocks_per_group;
    uint32_t free_blocks = 0;
    for (uint32_t bit = 0; bit < valid_blocks; bit++) if (!check_bitmap_bit(bitmap, bit)) free_blocks++;
    for (uint32_t bit = valid_blocks; bit < mnt->blocks_per_group; bit++) {
        if (!check_bitmap_bit(bitmap, bit)) {
            free(bitmap);
            return -EIO;
        }
    }
    uint32_t stored_free = read_le16(desc + 0x0C);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) stored_free |= (uint32_t)read_le16(desc + 0x2C) << 16;
    if (stored_free != free_blocks) {
        free(bitmap);
        return -EIO;
    }
    if (context->group_count == context->group_capacity) {
        size_t capacity = context->group_capacity ? context->group_capacity * 2 : 8;
        ext4_group_state_t *groups = malloc(capacity * sizeof(*groups));
        if (!groups) {
            free(bitmap);
            return -ENOMEM;
        }
        if (context->group_count) memcpy(groups, context->groups, context->group_count * sizeof(*groups));
        context->groups = groups;
        context->group_capacity = capacity;
    }
    ext4_group_state_t *state = &context->groups[context->group_count++];
    memcpy(state->descriptor, desc, mnt->desc_size);
    state->bitmap = bitmap;
    state->bitmap_block = bitmap_block;
    state->group = group;
    state->free_blocks = free_blocks;
    return 0;
}

static int allocate_ext4_run(const ext4_mount_t *mnt, ext4_write_context_t *context, uint32_t goal_group, uint32_t wanted, uint64_t *physical, uint32_t *length, uint32_t *allocated_group) {
    if (!wanted || !physical || !length || !allocated_group) return -EINVAL;
    if (wanted > EXT4_MAX_WRITTEN_EXTENT) wanted = EXT4_MAX_WRITTEN_EXTENT;
    bool unsupported = false;
    for (uint64_t visit = 0; visit < mnt->groups_count; visit++) {
        uint32_t group = (uint32_t)((goal_group + visit) % mnt->groups_count);
        int status = load_ext4_group(mnt, context, group);
        if (status == -EOPNOTSUPP) {
            unsupported = true;
            continue;
        }
        if (status < 0) return status;
        ext4_group_state_t *state = &context->groups[context->group_count - 1];
        for (size_t i = 0; i < context->group_count; i++) if (context->groups[i].group == group) state = &context->groups[i];
        uint64_t group_start = mnt->first_data_block + (uint64_t)group * mnt->blocks_per_group;
        uint64_t available = mnt->blocks_count - group_start;
        uint32_t valid_blocks = available < mnt->blocks_per_group ? (uint32_t)available : mnt->blocks_per_group;
        uint32_t start_bit = 0;
        if (!group && mnt->reserved_blocks) {
            uint64_t reserved_end = mnt->first_data_block + mnt->reserved_blocks;
            if (reserved_end > group_start) start_bit = (uint32_t)(reserved_end - group_start);
        }
        if (start_bit >= valid_blocks) continue;
        uint32_t run_start = 0;
        uint32_t run_length = 0;
        uint32_t best_start = 0;
        uint32_t best_length = 0;
        for (uint32_t bit = start_bit; bit < valid_blocks; bit++) {
            if (check_bitmap_bit(state->bitmap, bit)) {
                run_length = 0;
                continue;
            }
            if (!run_length) run_start = bit;
            run_length++;
            if (run_length > best_length) {
                best_start = run_start;
                best_length = run_length;
            }
        }
        if (!best_length) continue;
        uint32_t allocated = best_length < wanted ? best_length : wanted;
        uint64_t start = group_start + best_start;
        if (start > 0xFFFFFFFFFFFFULL || allocated > mnt->blocks_count - start) continue;
        for (uint32_t i = 0; i < allocated; i++) set_bitmap_bit(state->bitmap, best_start + i);
        state->free_blocks -= allocated;
        *physical = start;
        *length = allocated;
        *allocated_group = group;
        return 0;
    }
    return unsupported ? -EOPNOTSUPP : -ENOSPC;
}

static int insert_ext4_extent(uint8_t root[EXT4_N_BLOCKS * 4U], uint32_t logical, uint64_t physical, uint32_t length) {
    if (read_le16(root) != EXT4_EXTENT_MAGIC || read_le16(root + 6) != 0) return -EIO;
    uint16_t entries = read_le16(root + 2);
    uint16_t maximum = read_le16(root + 4);
    if (maximum > EXT4_ROOT_EXTENT_ENTRIES || entries > maximum || !length || length > EXT4_MAX_WRITTEN_EXTENT) return -EIO;
    if (physical > 0xFFFFFFFFFFFFULL || (uint64_t)logical + length > EXT4_MAX_LOGICAL_BLOCK + 1ULL) return -EFBIG;
    uint64_t previous_end = 0;
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *entry = root + 12U + (size_t)i * 12U;
        uint32_t entry_logical = read_le32(entry);
        uint32_t entry_length = read_le16(entry + 4);
        if (entry_length > EXT4_MAX_WRITTEN_EXTENT) entry_length -= EXT4_MAX_WRITTEN_EXTENT;
        if (!entry_length || (uint64_t)entry_logical + entry_length > EXT4_MAX_LOGICAL_BLOCK + 1ULL) return -EIO;
        if (i && entry_logical < previous_end) return -EIO;
        if (logical < entry_logical + entry_length && logical + length > entry_logical) return -EIO;
        previous_end = entry_logical + entry_length;
    }
    if (entries >= maximum) return -ENOSPC;
    uint16_t position = 0;
    while (position < entries && read_le32(root + 12U + (size_t)position * 12U) < logical) position++;
    memmove(root + 12U + (size_t)(position + 1) * 12U, root + 12U + (size_t)position * 12U, (size_t)(entries - position) * 12U);
    uint8_t *entry = root + 12U + (size_t)position * 12U;
    write_le32(entry, logical);
    write_le16(entry + 4, length);
    write_le16(entry + 6, physical >> 32);
    write_le32(entry + 8, physical);
    write_le16(root + 2, entries + 1);
    return 0;
}

static int plan_ext4_write(const ext4_mount_t *mnt, uint32_t ino, const ext4_inode_t *inode, uint64_t count, uint64_t offset, uint64_t *new_size, ext4_write_context_t *context) {
    uint32_t unsupported = EXT4_COMPR_FL | EXT4_ENCRYPT_FL | EXT4_VERITY_FL | EXT4_INLINE_DATA_FL | EXT4_JOURNAL_DATA_FL | EXT4_HUGE_FILE_FL | EXT4_SNAPFILE_FL | EXT4_SNAPFILE_DELETED_FL | EXT4_SNAPFILE_SHRUNK_FL;
    if (inode->flags & unsupported) return -EOPNOTSUPP;
    if (inode->deleted) return -ESTALE;
    if (!count) {
        *new_size = inode->size;
        return 0;
    }
    if (count > UINT64_MAX - offset) return -EOVERFLOW;
    uint64_t end = offset + count;
    uint64_t first64 = offset / mnt->block_size;
    uint64_t last64 = (end - 1) / mnt->block_size;
    if (first64 > UINT32_MAX || last64 > EXT4_MAX_LOGICAL_BLOCK || last64 < first64) return -EFBIG;
    if (last64 - first64 + 1 > SIZE_MAX / sizeof(*context->blocks)) return -EFBIG;
    memset(context, 0, sizeof(*context));
    context->first_block = (uint32_t)first64;
    context->block_count = (size_t)(last64 - first64 + 1);
    context->blocks = calloc(context->block_count, sizeof(*context->blocks));
    if (!context->blocks) return -ENOMEM;
    memcpy(context->extents, inode->block, sizeof(context->extents));
    bool extents = inode->flags & EXT4_EXTENTS_FL;
    bool shallow_extents = extents && read_le16(inode->block + 6) == 0;
    uint32_t goal_group = (ino - 1) / mnt->inodes_per_group;
    size_t index = 0;
    while (index < context->block_count) {
        uint32_t logical = context->first_block + (uint32_t)index;
        uint64_t physical;
        int mapped = map_ext4_inode_block(mnt, inode, logical, &physical);
        if (mapped < 0) return mapped;
        if (mapped) {
            context->blocks[index].physical = physical;
            context->blocks[index].initialize = mapped == 2;
            index++;
            continue;
        }
        if (!shallow_extents) return -EOPNOTSUPP;
        uint32_t hole_end = logical + 1;
        while (hole_end <= last64) {
            int status = map_ext4_inode_block(mnt, inode, hole_end, &physical);
            if (status < 0) return status;
            if (status) break;
            hole_end++;
        }
        uint32_t wanted = hole_end - logical;
        if (wanted > EXT4_MAX_WRITTEN_EXTENT) wanted = EXT4_MAX_WRITTEN_EXTENT;
        while (wanted) {
            uint32_t allocated_group;
            uint64_t start;
            uint32_t allocated;
            int status = allocate_ext4_run(mnt, context, goal_group, wanted, &start, &allocated, &allocated_group);
            if (status < 0) return status;
            status = insert_ext4_extent(context->extents, logical, start, allocated);
            if (status < 0) return status;
            for (uint32_t i = 0; i < allocated; i++) {
                context->blocks[index + i].physical = start + i;
                context->blocks[index + i].allocated = true;
                context->blocks[index + i].initialize = true;
            }
            context->allocated_blocks += allocated;
            logical += allocated;
            index += allocated;
            wanted -= allocated;
            goal_group = allocated_group;
        }
    }
    *new_size = end > inode->size ? end : inode->size;
    return 0;
}

static int write_ext4_data(const ext4_mount_t *mnt, const void *buffer, uint64_t count, uint64_t offset, const ext4_write_context_t *context) {
    const uint8_t *input = buffer;
    uint8_t *block = NULL;
    int result = 0;
    for (size_t i = 0; i < context->block_count; i++) {
        uint32_t logical = context->first_block + (uint32_t)i;
        uint64_t source_offset = (uint64_t)(logical - context->first_block) * mnt->block_size;
        uint32_t in_block = (uint32_t)((offset + source_offset) % mnt->block_size);
        uint32_t chunk = mnt->block_size - in_block;
        uint64_t remaining = count - source_offset;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        if (chunk > mnt->block_size) chunk = mnt->block_size;
        uint64_t physical_offset = context->blocks[i].physical * (uint64_t)mnt->block_size + in_block;
        if ((!context->blocks[i].initialize || in_block || chunk != mnt->block_size) && !block) {
            block = malloc(mnt->block_size);
            if (!block) {
                result = -ENOMEM;
                break;
            }
        }
        if (context->blocks[i].initialize && (in_block || chunk != mnt->block_size)) {
            memset(block, 0, mnt->block_size);
            memcpy(block + in_block, input + source_offset, chunk);
            result = write_ext4_block(mnt, context->blocks[i].physical, block);
        } else if (in_block || chunk != mnt->block_size) {
            result = read_ext4_block(mnt, context->blocks[i].physical, block);
            if (result == 0) {
                memcpy(block + in_block, input + source_offset, chunk);
                result = write_ext4_block(mnt, context->blocks[i].physical, block);
            }
        } else {
            result = write_checked_device(mnt, input + source_offset, chunk, physical_offset);
        }
        if (result < 0) break;
    }
    free(block);
    return result;
}

static void free_ext4_write_context(ext4_write_context_t *context) {
    if (!context) return;
    for (size_t i = 0; i < context->group_count; i++) free(context->groups[i].bitmap);
    free(context->groups);
    free(context->blocks);
    memset(context, 0, sizeof(*context));
}

static int commit_ext4_write(ext4_mount_t *mnt, uint32_t ino, const ext4_inode_location_t *location, uint8_t *raw, uint64_t new_size, const ext4_write_context_t *context) {
    uint8_t super[EXT4_SUPER_SIZE];
    int status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status < 0) return status;
    status = validate_superblock_checksum(mnt, super);
    if (status < 0) return status;
    if (!mnt->has_journal) {
        uint16_t state = read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN;
        write_le16(super + 0x3A, state);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status < 0) return status;
        if (context->allocated_blocks) {
            uint32_t bitmap_bytes = mnt->blocks_per_group >> 3;
            for (size_t i = 0; i < context->group_count; i++) {
                ext4_group_state_t *group = &context->groups[i];
                if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
                    uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), group->bitmap, bitmap_bytes);
                    write_le16(group->descriptor + 0x18, checksum);
                    if (mnt->desc_size >= 64) write_le16(group->descriptor + 0x38, checksum >> 16);
                }
                write_le16(group->descriptor + 0x0C, group->free_blocks);
                if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) write_le16(group->descriptor + 0x2C, 0);
                update_group_checksum(mnt, group->group, group->descriptor);
                status = write_ext4_block(mnt, group->bitmap_block, group->bitmap);
                if (status < 0) return status;
                status = write_ext4_group_desc(mnt, group->group, group->descriptor);
                if (status < 0) return status;
            }
        }
    }
    memcpy(raw + 40, context->extents, 60);
    write_le32(raw + 4, new_size);
    write_le32(raw + 0x6C, new_size >> 32);
    uint64_t added_blocks = (uint64_t)context->allocated_blocks * (mnt->block_size / 512U);
    uint64_t blocks_512 = read_le32(raw + 0x1C);
    if (added_blocks > UINT64_MAX - blocks_512) return -EFBIG;
    write_le32(raw + 0x1C, blocks_512 + added_blocks);
    struct timespec now = time_get_realtime_ts();
    if (now.tv_sec >= 0 && now.tv_sec <= UINT32_MAX) {
        write_le32(raw + 0x0C, now.tv_sec);
        write_le32(raw + 0x10, now.tv_sec);
        uint16_t extra_size = mnt->inode_size >= 132 ? read_le16(raw + 0x80) : 0;
        if (extra_size >= 12 && now.tv_nsec >= 0 && now.tv_nsec < 1000000000L) {
            uint32_t extra = (uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U) | ((uint32_t)now.tv_nsec << 2);
            write_le32(raw + 0x84, extra);
            write_le32(raw + 0x88, extra);
        }
    }
    update_inode_checksum(mnt, ino, raw);
    if (mnt->has_journal) {
        if (context->allocated_blocks) {
            finalize_group_states(mnt, context);
            if (context->allocated_blocks > mnt->free_blocks) return -ENOSPC;
            uint64_t free_blocks = mnt->free_blocks - context->allocated_blocks;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
        }
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        ext4_journal_batch_t batch = {0};
        status = stage_superblock(mnt, &batch, super);
        for (size_t i = 0; i < context->group_count && status == 0; i++) {
            status = stage_full_block(mnt, &batch, context->groups[i].bitmap_block, context->groups[i].bitmap);
            if (status == 0) status = stage_group_descriptor(mnt, &batch, context->groups[i].group, context->groups[i].descriptor);
        }
        if (status == 0) status = stage_inode_bytes(mnt, &batch, location, raw, mnt->inode_size);
        if (status == 0) status = commit_staged_blocks(mnt, &batch);
        free_staged_batch(&batch);
        if (status < 0) return status;
        mnt->free_blocks -= context->allocated_blocks;
        mnt->state = read_le16(super + 0x3A);
        return 0;
    }
    status = write_ext4_inode_bytes(mnt, location, raw);
    if (status < 0) return status;
    if (context->allocated_blocks) {
        if (context->allocated_blocks > mnt->free_blocks) return -ENOSPC;
        uint64_t free_blocks = mnt->free_blocks - context->allocated_blocks;
        write_le32(super + 0x0C, free_blocks);
        if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, free_blocks >> 32);
    }
    write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
    update_superblock_checksum(mnt, super);
    status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status < 0) return status;
    mnt->free_blocks -= context->allocated_blocks;
    mnt->state = read_le16(super + 0x3A);
    return 0;
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static void write_be16(uint8_t *p, uint16_t value) {
    p[0] = (value >> 8) & 0xFF;
    p[1] = value & 0xFF;
}

static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (value >> 24) & 0xFF;
    p[1] = (value >> 16) & 0xFF;
    p[2] = (value >> 8) & 0xFF;
    p[3] = value & 0xFF;
}

static ext4_journal_record_t *find_staged_record(ext4_journal_batch_t *batch, uint64_t fs_block) {
    for (size_t i = 0; i < batch->count; i++) if (batch->records[i].fs_block == fs_block) return &batch->records[i];
    return NULL;
}

static int stage_fs_block(ext4_mount_t *mnt, ext4_journal_batch_t *batch, uint64_t fs_block) {
    if (find_staged_record(batch, fs_block)) return 0;
    if (batch->count == batch->capacity) {
        size_t capacity = batch->capacity ? batch->capacity * 2 : 8;
        ext4_journal_record_t *records = malloc(capacity * sizeof(*records));
        if (!records) return -ENOMEM;
        if (batch->count) memcpy(records, batch->records, batch->count * sizeof(*records));
        free(batch->records);
        batch->records = records;
        batch->capacity = capacity;
    }
    uint8_t *data = malloc(mnt->block_size);
    if (!data) return -ENOMEM;
    int status = read_ext4_block(mnt, fs_block, data);
    if (status < 0) { free(data); return status; }
    batch->records[batch->count].fs_block = fs_block;
    batch->records[batch->count].data = data;
    batch->count++;
    return 0;
}

static int patch_staged_block(ext4_mount_t *mnt, ext4_journal_batch_t *batch, uint64_t fs_block, uint64_t block_offset, const void *data, uint64_t length) {
    if (block_offset > mnt->block_size || length > mnt->block_size - block_offset) return -EIO;
    int status = stage_fs_block(mnt, batch, fs_block);
    if (status < 0) return status;
    memcpy(find_staged_record(batch, fs_block)->data + block_offset, data, (size_t)length);
    return 0;
}

static int stage_full_block(ext4_mount_t *mnt, ext4_journal_batch_t *batch, uint64_t fs_block, const void *data) {
    return patch_staged_block(mnt, batch, fs_block, 0, data, mnt->block_size);
}

static void free_staged_batch(ext4_journal_batch_t *batch) {
    if (!batch) return;
    for (size_t i = 0; i < batch->count; i++) free(batch->records[i].data);
    free(batch->records);
    memset(batch, 0, sizeof(*batch));
}

static int stage_superblock(ext4_mount_t *mnt, ext4_journal_batch_t *batch, const uint8_t super[EXT4_SUPER_SIZE]) {
    return patch_staged_block(mnt, batch, EXT4_SUPER_OFFSET / mnt->block_size, EXT4_SUPER_OFFSET % mnt->block_size, super, EXT4_SUPER_SIZE);
}

static int stage_group_descriptor(ext4_mount_t *mnt, ext4_journal_batch_t *batch, uint32_t group, const uint8_t desc[64]) {
    uint64_t byte = mnt->gdt_offset + (uint64_t)group * mnt->desc_size;
    return patch_staged_block(mnt, batch, byte / mnt->block_size, byte % mnt->block_size, desc, mnt->desc_size);
}

static int stage_inode_bytes(ext4_mount_t *mnt, ext4_journal_batch_t *batch, const ext4_inode_location_t *location, const uint8_t *raw, uint16_t size) {
    uint64_t done = 0;
    while (done < size) {
        uint64_t byte = location->byte_offset + done;
        uint64_t chunk = mnt->block_size - byte % mnt->block_size;
        if (chunk > size - done) chunk = size - done;
        int status = patch_staged_block(mnt, batch, byte / mnt->block_size, byte % mnt->block_size, raw + done, chunk);
        if (status < 0) return status;
        done += chunk;
    }
    return 0;
}

static int read_journal_block(ext4_mount_t *mnt, const ext4_inode_t *inode, uint32_t journal_block, void *buffer) {
    uint64_t physical;
    int mapped = map_ext4_inode_block(mnt, inode, journal_block, &physical);
    if (mapped < 0) return mapped;
    if (mapped != 1) return -EIO;
    return read_ext4_block(mnt, physical, buffer);
}

static int write_journal_block(ext4_mount_t *mnt, const ext4_inode_t *inode, uint32_t journal_block, const void *buffer) {
    uint64_t physical;
    int mapped = map_ext4_inode_block(mnt, inode, journal_block, &physical);
    if (mapped < 0) return mapped;
    if (mapped != 1) return -EIO;
    return write_ext4_block(mnt, physical, buffer);
}

static uint32_t journal_next_block(const ext4_mount_t *mnt, uint32_t head) {
    head++;
    if (head >= mnt->journal_maxlen) head = mnt->journal_first;
    return head;
}

static int read_journal_super(ext4_mount_t *mnt, const ext4_inode_t *inode, uint8_t *jsb) {
    int status = read_journal_block(mnt, inode, 0, jsb);
    if (status < 0) return status;
    if (read_be32(jsb) != EXT4_JOURNAL_MAGIC) return -EIO;
    if (read_be32(jsb + 4) != EXT4_JOURNAL_SUPER_V2) return -EOPNOTSUPP;
    if (read_be32(jsb + 12) != mnt->block_size) return -EOPNOTSUPP;
    if (read_be32(jsb + 36) || read_be32(jsb + 40) || read_be32(jsb + 44)) return -EOPNOTSUPP;
    return 0;
}

static int write_journal_transaction(ext4_mount_t *mnt, const ext4_inode_t *inode, ext4_journal_batch_t *batch, uint32_t sequence, uint32_t *head) {
    size_t tags_per_desc = (mnt->block_size - 12 - 16) / 8;
    if (!tags_per_desc || !batch->count) return -ENOSPC;
    size_t descs = (batch->count + tags_per_desc - 1) / tags_per_desc;
    if (batch->count + descs + 1 > mnt->journal_maxlen - mnt->journal_first) return -ENOSPC;
    uint8_t *desc = malloc(mnt->block_size);
    if (!desc) return -ENOMEM;
    uint8_t *escaped = malloc(mnt->block_size);
    if (!escaped) { free(desc); return -ENOMEM; }
    size_t index = 0;
    uint32_t cursor = *head;
    int status = 0;
    while (status == 0 && index < batch->count) {
        size_t chunk = batch->count - index;
        if (chunk > tags_per_desc) chunk = tags_per_desc;
        bool last_desc = index + chunk == batch->count;
        memset(desc, 0, mnt->block_size);
        write_be32(desc, EXT4_JOURNAL_MAGIC);
        write_be32(desc + 4, EXT4_JOURNAL_DESCRIPTOR);
        write_be32(desc + 8, sequence);
        size_t offset = 12;
        for (size_t i = 0; i < chunk; i++) {
            uint64_t fs_block = batch->records[index + i].fs_block;
            if (fs_block > UINT32_MAX) { status = -EFBIG; break; }
            uint16_t tag_flags = EXT4_JOURNAL_TAG_SAME_UUID;
            if (index + i == 0) tag_flags = 0;
            if (read_be32(batch->records[index + i].data) == EXT4_JOURNAL_MAGIC) tag_flags |= EXT4_JOURNAL_TAG_ESCAPE;
            if (last_desc && i + 1 == chunk) tag_flags |= EXT4_JOURNAL_TAG_LAST;
            write_be32(desc + offset, (uint32_t)fs_block);
            write_be16(desc + offset + 4, 0);
            write_be16(desc + offset + 6, tag_flags);
            offset += 8;
            if (index + i == 0) {
                memcpy(desc + offset, mnt->journal_uuid, 16);
                offset += 16;
            }
        }
        if (status == 0) status = write_journal_block(mnt, inode, cursor, desc);
        cursor = journal_next_block(mnt, cursor);
        for (size_t i = 0; i < chunk && status == 0; i++) {
            const uint8_t *data = batch->records[index + i].data;
            if (read_be32(data) == EXT4_JOURNAL_MAGIC) {
                memcpy(escaped, data, mnt->block_size);
                write_be32(escaped, 0);
                data = escaped;
            }
            status = write_journal_block(mnt, inode, cursor, data);
            cursor = journal_next_block(mnt, cursor);
        }
        index += chunk;
    }
    if (status == 0) {
        memset(desc, 0, mnt->block_size);
        write_be32(desc, EXT4_JOURNAL_MAGIC);
        write_be32(desc + 4, EXT4_JOURNAL_COMMIT);
        write_be32(desc + 8, sequence);
        status = write_journal_block(mnt, inode, cursor, desc);
        cursor = journal_next_block(mnt, cursor);
    }
    free(escaped);
    free(desc);
    if (status == 0) *head = cursor;
    return status;
}

static int journal_begin_transaction(ext4_mount_t *mnt, const ext4_inode_t *inode, uint8_t *jsb, uint32_t sequence, uint32_t head) {
    int status = read_journal_super(mnt, inode, jsb);
    if (status < 0) return status;
    write_be32(jsb + 24, sequence);
    write_be32(jsb + 28, head);
    return write_journal_block(mnt, inode, 0, jsb);
}

static int journal_end_transaction(ext4_mount_t *mnt, const ext4_inode_t *inode, uint8_t *jsb, uint32_t sequence, uint32_t head) {
    int status = read_journal_super(mnt, inode, jsb);
    if (status < 0) return status;
    uint32_t next = sequence + 1;
    if (!next) next = 1;
    write_be32(jsb + 24, next);
    write_be32(jsb + 28, 0);
    write_be32(jsb + 88, head);
    return write_journal_block(mnt, inode, 0, jsb);
}

static int commit_staged_blocks(ext4_mount_t *mnt, ext4_journal_batch_t *batch) {
    if (!batch->count) return 0;
    if (!mnt->has_journal) {
        for (size_t i = 0; i < batch->count; i++) {
            int status = write_ext4_block(mnt, batch->records[i].fs_block, batch->records[i].data);
            if (status < 0) { mnt->read_only = true; return status; }
        }
        return 0;
    }
    ext4_inode_t journal_inode;
    int status = read_ext4_inode(mnt, mnt->journal_ino, &journal_inode);
    if (status < 0) return status;
    size_t tags_per_desc = (mnt->block_size - 12 - 16) / 8;
    if (!tags_per_desc) return -ENOSPC;
    size_t descs = (batch->count + tags_per_desc - 1) / tags_per_desc;
    if (batch->count + descs + 1 > (size_t)mnt->journal_maxlen - mnt->journal_first) return -ENOSPC;
    uint8_t *jsb = malloc(mnt->block_size);
    if (!jsb) return -ENOMEM;
    uint32_t sequence = mnt->journal_sequence;
    uint32_t head = mnt->journal_head;
    if (!sequence) sequence = 1;
    status = journal_begin_transaction(mnt, &journal_inode, jsb, sequence, head);
    if (status == 0) status = write_journal_transaction(mnt, &journal_inode, batch, sequence, &head);
    if (status == 0) {
        for (size_t i = 0; i < batch->count; i++) {
            status = write_ext4_block(mnt, batch->records[i].fs_block, batch->records[i].data);
            if (status < 0) break;
        }
    }
    if (status == 0) status = journal_end_transaction(mnt, &journal_inode, jsb, sequence, head);
    if (status == 0) {
        uint32_t next = sequence + 1;
        if (!next) next = 1;
        mnt->journal_sequence = next;
        mnt->journal_head = head;
    } else mnt->read_only = true;
    free(jsb);
    return status;
}

static int replay_journal_descriptor(ext4_mount_t *mnt, const ext4_inode_t *inode, const uint8_t *desc, uint32_t *head, ext4_journal_batch_t *batch, uint8_t *data) {
    size_t offset = 12;
    while (1) {
        if (offset + 8 > mnt->block_size) return -EUCLEAN;
        uint32_t fs_block = read_be32(desc + offset);
        uint16_t tag_flags = read_be16(desc + offset + 6);
        offset += 8;
        if (!(tag_flags & EXT4_JOURNAL_TAG_SAME_UUID)) offset += 16;
        if (tag_flags & ~(EXT4_JOURNAL_TAG_ESCAPE | EXT4_JOURNAL_TAG_SAME_UUID | EXT4_JOURNAL_TAG_DELETED | EXT4_JOURNAL_TAG_LAST)) return -EUCLEAN;
        if (fs_block >= mnt->blocks_count) return -EUCLEAN;
        *head = journal_next_block(mnt, *head);
        int status = read_journal_block(mnt, inode, *head, data);
        if (status < 0) return status;
        if (tag_flags & EXT4_JOURNAL_TAG_ESCAPE) write_be32(data, EXT4_JOURNAL_MAGIC);
        status = stage_full_block(mnt, batch, fs_block, data);
        if (status < 0) return status;
        *head = journal_next_block(mnt, *head);
        if (tag_flags & EXT4_JOURNAL_TAG_LAST) return 0;
    }
}

static int replay_journal(ext4_mount_t *mnt, const ext4_inode_t *inode, const uint8_t *jsb, uint32_t *next_sequence) {
    uint32_t maxlen = read_be32(jsb + 16);
    uint32_t first = read_be32(jsb + 20);
    uint32_t start = read_be32(jsb + 28);
    if (!maxlen || !first || first >= maxlen || start >= maxlen) return -EUCLEAN;
    uint8_t *block = malloc(mnt->block_size);
    if (!block) return -ENOMEM;
    uint8_t *data = malloc(mnt->block_size);
    if (!data) { free(block); return -ENOMEM; }
    ext4_journal_batch_t batch = {0};
    uint32_t head = start;
    uint32_t expected = 0;
    bool started = false;
    int result = 0;
    for (size_t iterations = 0; result == 0 && iterations < (size_t)maxlen * 2 + 8; iterations++) {
        result = read_journal_block(mnt, inode, head, block);
        if (result < 0) break;
        if (read_be32(block) != EXT4_JOURNAL_MAGIC) { result = started ? 0 : -EUCLEAN; break; }
        uint32_t blocktype = read_be32(block + 4);
        uint32_t blockseq = read_be32(block + 8);
        if (!started) {
            if (blocktype != EXT4_JOURNAL_DESCRIPTOR) { result = -EUCLEAN; break; }
            expected = blockseq;
            started = true;
        }
        if (blockseq != expected) break;
        if (blocktype == EXT4_JOURNAL_DESCRIPTOR) {
            result = replay_journal_descriptor(mnt, inode, block, &head, &batch, data);
            if (result < 0) break;
            continue;
        }
        if (blocktype == EXT4_JOURNAL_COMMIT) {
            for (size_t i = 0; i < batch.count && result == 0; i++) result = write_ext4_block(mnt, batch.records[i].fs_block, batch.records[i].data);
            free_staged_batch(&batch);
            if (result < 0) break;
            expected++;
            head = journal_next_block(mnt, head);
            continue;
        }
        result = -EUCLEAN;
        break;
    }
    free_staged_batch(&batch);
    free(data);
    free(block);
    if (result == 0 && started) *next_sequence = expected;
    if (result == 0 && !started) result = -EUCLEAN;
    return result;
}

static int check_ext4_orphans(ext4_mount_t *mnt, const uint8_t super[EXT4_SUPER_SIZE]) {
    uint32_t orphan_ino = read_le32(super + 0x280);
    if (!orphan_ino) return 0;
    if (orphan_ino > mnt->inodes_count) return -EIO;
    ext4_inode_t inode;
    int status = read_ext4_inode(mnt, orphan_ino, &inode);
    if (status < 0) return status;
    if ((inode.mode & S_IFMT) != S_IFREG) return -EIO;
    if (inode.size > 1024 * 1024) return -EUCLEAN;
    uint8_t *block = malloc(mnt->block_size);
    if (!block) return -ENOMEM;
    uint64_t blocks = (inode.size + mnt->block_size - 1) / mnt->block_size;
    int result = 0;
    for (uint64_t logical = 0; logical < blocks && result == 0; logical++) {
        if (logical > UINT32_MAX) { result = -EFBIG; break; }
        uint64_t physical;
        int mapped = map_ext4_inode_block(mnt, &inode, (uint32_t)logical, &physical);
        if (mapped < 0) { result = mapped; break; }
        if (!mapped) continue;
        result = read_ext4_block(mnt, physical, block);
        if (result < 0) break;
        uint32_t slots = (mnt->block_size - 8) / 4;
        for (uint32_t i = 0; i < slots; i++) if (read_le32(block + (size_t)i * 4U) != 0) { result = -EUCLEAN; break; }
    }
    free(block);
    return result;
}

static int open_ext4_journal(ext4_mount_t *mnt, const uint8_t super[EXT4_SUPER_SIZE]) {
    uint32_t journal_ino = read_le32(super + 0xE0);
    uint32_t journal_dev = read_le32(super + 0xE4);
    if (!journal_ino || journal_dev || journal_ino > mnt->inodes_count) return -EOPNOTSUPP;
    if (mnt->blocks_count > UINT32_MAX) return -EOPNOTSUPP;
    ext4_inode_t journal_inode;
    int status = read_ext4_inode(mnt, journal_ino, &journal_inode);
    if (status < 0) return status;
    if ((journal_inode.mode & S_IFMT) != S_IFREG) return -EIO;
    uint8_t *jsb = malloc(mnt->block_size);
    if (!jsb) return -ENOMEM;
    status = read_journal_super(mnt, &journal_inode, jsb);
    uint32_t maxlen = 0;
    uint32_t first = 0;
    uint32_t sequence = 0;
    uint32_t start = 0;
    if (status == 0) {
        maxlen = read_be32(jsb + 16);
        first = read_be32(jsb + 20);
        sequence = read_be32(jsb + 24);
        start = read_be32(jsb + 28);
        if (!maxlen || !first || first >= maxlen || start >= maxlen || maxlen > mnt->blocks_count) status = -EIO;
        else if (read_be32(jsb + 32)) status = -EUCLEAN;
        else if (!sequence) status = -EUCLEAN;
    }
    uint32_t next = sequence;
    if (status == 0 && start) {
        status = replay_journal(mnt, &journal_inode, jsb, &next);
        if (status == 0) {
            write_be32(jsb + 24, next);
            write_be32(jsb + 28, 0);
            write_be32(jsb + 88, first);
            status = write_journal_block(mnt, &journal_inode, 0, jsb);
        }
    }
    if (status == 0) {
        mnt->has_journal = true;
        mnt->journal_ino = journal_ino;
        mnt->journal_maxlen = maxlen;
        mnt->journal_first = first;
        mnt->journal_sequence = next ? next : 1;
        mnt->journal_head = first;
        memcpy(mnt->journal_uuid, jsb + 48, 16);
    }
    free(jsb);
    return status;
}

int64_t write_ext4(const char *path, const void *buffer, uint64_t count, uint64_t offset, bool append) {
    if (!path || (!buffer && count)) return -EINVAL;
    if (!count) return 0;
    if (count > INT64_MAX) return -EFBIG;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return -ENOENT;
    }
    if (mnt->read_only) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return -EROFS;
    }
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, true, &ino, &inode);
    if (status < 0) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (!S_ISREG(inode.mode)) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return S_ISDIR(inode.mode) ? -EISDIR : -EINVAL;
    }
    if (inode.flags & EXT4_IMMUTABLE_FL) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return -EPERM;
    }
    if (append) offset = inode.size;
    if ((inode.flags & EXT4_APPEND_FL) && !append && offset != inode.size) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return -EPERM;
    }
    ext4_inode_location_t location;
    status = locate_ext4_inode(mnt, ino, &location);
    if (status < 0) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    uint8_t *raw = malloc(mnt->inode_size);
    if (!raw) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return -ENOMEM;
    }
    status = read_ext4_inode_bytes(mnt, ino, raw);
    if (status < 0) {
        free(raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    uint64_t new_size;
    ext4_write_context_t context = {0};
    bool committed = false;
    status = plan_ext4_write(mnt, ino, &inode, count, offset, &new_size, &context);
    if (status == 0) status = write_ext4_data(mnt, buffer, count, offset, &context);
    if (status == 0) {
        status = commit_ext4_write(mnt, ino, &location, raw, new_size, &context);
        committed = true;
    }
    if (status < 0 && committed) mnt->read_only = true;
    free_ext4_write_context(&context);
    free(raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status < 0 ? status : (int64_t)count;
}

static int split_ext4_parent(const char *path, char parent[256], char name[256]) {
    if (!path || path[0] != '/') return -EINVAL;
    char work[256];
    strlcpy(work, path, sizeof(work));
    size_t length = strlen(work);
    while (length > 1 && work[length - 1] == '/') work[--length] = '\0';
    char *slash = strrchr(work, '/');
    if (!slash) return -EINVAL;
    if (slash == work) strlcpy(parent, "/", 256);
    else {
        size_t parent_length = (size_t)(slash - work);
        if (parent_length >= 256) return -ENAMETOOLONG;
        memcpy(parent, work, parent_length);
        parent[parent_length] = '\0';
    }
    if (slash[1] == '\0' || strlen(slash + 1) > 255) return -ENAMETOOLONG;
    strlcpy(name, slash + 1, 256);
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EINVAL;
    return 0;
}

static int map_ext4_parent_block(const ext4_mount_t *mnt, const uint8_t *raw, uint32_t logical, uint64_t *physical) {
    if (read_le32(raw + 32) & EXT4_EXTENTS_FL) {
        if (read_le16(raw + 40) != EXT4_EXTENT_MAGIC || read_le16(raw + 46) != 0) return -EIO;
        uint16_t entries = read_le16(raw + 42);
        uint16_t maximum = read_le16(raw + 44);
        if (entries > maximum || maximum > EXT4_ROOT_EXTENT_ENTRIES) return -EIO;
        for (uint16_t i = 0; i < entries; i++) {
            const uint8_t *entry = raw + 52U + (size_t)i * 12U;
            uint32_t first = read_le32(entry);
            uint16_t raw_length = read_le16(entry + 4);
            if (raw_length > EXT4_MAX_WRITTEN_EXTENT) return -EIO;
            if (!raw_length || logical < first || logical - first >= raw_length) continue;
            uint64_t start = (uint64_t)read_le32(entry + 8) | ((uint64_t)read_le16(entry + 6) << 32);
            if (!start || start >= mnt->blocks_count || logical - first >= mnt->blocks_count - start) return -EIO;
            *physical = start + (logical - first);
            return 1;
        }
        return 0;
    }
    if (logical >= 12) return -EOPNOTSUPP;
    uint32_t block = read_le32(raw + 40 + logical * 4U);
    if (block >= mnt->blocks_count) return -EIO;
    *physical = block;
    return block != 0;
}

static void update_ext4_dir_checksum(ext4_mount_t *mnt, uint32_t ino, uint32_t generation, uint8_t *block) {
    uint8_t ino_bytes[4];
    uint8_t gen_bytes[4];
    write_le32(ino_bytes, ino);
    write_le32(gen_bytes, generation);
    uint32_t checksum = calculate_ext4_seed(mnt);
    checksum = calculate_crc32c(checksum, ino_bytes, sizeof(ino_bytes));
    checksum = calculate_crc32c(checksum, gen_bytes, sizeof(gen_bytes));
    checksum = calculate_crc32c(checksum, block, mnt->block_size - 12);
    write_le32(block + mnt->block_size - 4, checksum);
}

static int finalize_ext4_dir_block(ext4_mount_t *mnt, uint32_t ino, const uint8_t *raw, uint8_t *block) {
    if (!(mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return 0;
    uint32_t generation = read_le32(raw + 100);
    uint32_t pos = 0;
    uint64_t live_total = 0;
    while (pos < mnt->block_size) {
        if (pos + 8 > mnt->block_size) return -EIO;
        uint32_t entry_ino = read_le32(block + pos);
        uint16_t rec_len = read_le16(block + pos + 4);
        uint8_t name_length = block[pos + 6];
        if (rec_len < 8 || (rec_len & 3) || rec_len > mnt->block_size - pos) return -EIO;
        if (entry_ino) live_total += 8U + ((name_length + 3U) & ~3U);
        pos += rec_len;
    }
    if (pos != mnt->block_size || live_total > mnt->block_size - 12) return -EIO;
    uint32_t read_pos = 0;
    uint32_t write_pos = 0;
    uint32_t last_pos = 0;
    bool have_live = false;
    while (read_pos < mnt->block_size) {
        uint32_t entry_ino = read_le32(block + read_pos);
        uint16_t rec_len = read_le16(block + read_pos + 4);
        uint8_t name_length = block[read_pos + 6];
        if (!entry_ino) {
            read_pos += rec_len;
            continue;
        }
        uint32_t actual = 8U + ((name_length + 3U) & ~3U);
        memmove(block + write_pos, block + read_pos, 8U + name_length);
        memset(block + write_pos + 8 + name_length, 0, actual - 8 - name_length);
        last_pos = write_pos;
        have_live = true;
        write_pos += actual;
        read_pos += rec_len;
    }
    if (!have_live) {
        write_le32(block, 0);
        write_le16(block + 4, mnt->block_size - 12);
    } else write_le16(block + last_pos + 4, mnt->block_size - 12 - last_pos);
    write_le32(block + mnt->block_size - 12, 0);
    write_le16(block + mnt->block_size - 8, 12);
    block[mnt->block_size - 6] = 0;
    block[mnt->block_size - 5] = 0xDE;
    update_ext4_dir_checksum(mnt, ino, generation, block);
    return 0;
}

static int insert_ext4_dirent(ext4_mount_t *mnt, const uint8_t *parent_raw, const char *name, size_t name_length, uint32_t child, uint8_t file_type, uint64_t *dir_block, uint8_t *dir_data, uint64_t alias_block, const uint8_t *alias_data) {
    uint64_t parent_size = read_le32(parent_raw + 4);
    uint32_t needed = (8U + (uint32_t)name_length + 3U) & ~3U;
    if (!name || !name_length || name_length > 255 || !dir_block || !dir_data) return -EINVAL;
    if (needed > mnt->block_size) return -ENAMETOOLONG;
    uint8_t stored_type = (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) ? file_type : 0;
    for (uint64_t block_offset = 0; block_offset < parent_size; block_offset += mnt->block_size) {
        uint64_t logical64 = block_offset / mnt->block_size;
        if (logical64 > UINT32_MAX) return -EFBIG;
        uint64_t physical;
        int mapped = map_ext4_parent_block(mnt, parent_raw, (uint32_t)logical64, &physical);
        if (mapped < 0) return mapped;
        if (!mapped) continue;
        int status;
        if (alias_block && physical == alias_block) {
            if (!alias_data) return -EINVAL;
            memcpy(dir_data, alias_data, mnt->block_size);
            status = 0;
        } else status = read_ext4_block(mnt, physical, dir_data);
        if (status < 0) return status;
        uint32_t pos = 0;
        int result = -ENOSPC;
        while (pos < mnt->block_size) {
            if (pos + 8 > mnt->block_size) { result = -EIO; break; }
            uint32_t ino = read_le32(dir_data + pos);
            uint16_t rec_len = read_le16(dir_data + pos + 4);
            uint8_t current_length = dir_data[pos + 6];
            if (rec_len == 0 && ino == 0) rec_len = mnt->block_size - pos;
            if (rec_len < 8 || (rec_len & 3) || rec_len > mnt->block_size - pos) { result = -EIO; break; }
            uint32_t actual = 8U + ((current_length + 3U) & ~3U);
            if (ino != 0 && actual > rec_len) { result = -EIO; break; }
            if (ino != 0 && current_length == name_length && memcmp(dir_data + pos + 8, name, name_length) == 0) { result = -EEXIST; break; }
            uint32_t insert_at = ino == 0 ? pos : pos + actual;
            uint32_t slack = ino == 0 ? rec_len : rec_len - actual;
            if (slack >= needed) {
                if (ino != 0) write_le16(dir_data + pos + 4, (uint16_t)actual);
                uint32_t take = needed;
                uint32_t rest = slack - needed;
                if (rest != 0 && rest < 8) take = slack;
                write_le32(dir_data + insert_at, child);
                write_le16(dir_data + insert_at + 4, (uint16_t)take);
                dir_data[insert_at + 6] = (uint8_t)name_length;
                dir_data[insert_at + 7] = stored_type;
                memcpy(dir_data + insert_at + 8, name, name_length);
                if (take < slack) {
                    uint32_t free_at = insert_at + take;
                    write_le32(dir_data + free_at, 0);
                    write_le16(dir_data + free_at + 4, (uint16_t)(slack - take));
                }
                *dir_block = physical;
                return 0;
            }
            pos += rec_len;
        }
        if (result != -ENOSPC) return result;
    }
    return -ENOSPC;
}

static int allocate_ext4_inode(ext4_mount_t *mnt, uint32_t *ino, uint8_t desc[64], uint8_t **bitmap, uint64_t *bitmap_block, uint32_t *group) {
    if (!ino || !desc || !bitmap || !bitmap_block || !group) return -EINVAL;
    if (mnt->inodes_per_group & 7U) return -EOPNOTSUPP;
    uint32_t bitmap_bytes = mnt->inodes_per_group >> 3;
    if (bitmap_bytes > mnt->block_size) return -EIO;
    for (uint32_t current = 0; current < mnt->groups_count; current++) {
        int status = read_ext4_group_desc(mnt, current, desc);
        if (status < 0) return status;
        status = validate_group_checksum(mnt, current, desc);
        if (status < 0) return status;
        if (read_le16(desc + 0x12) & EXT4_BG_INODE_UNINIT) continue;
        uint64_t map_block = read_le32(desc + 4);
        if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) map_block |= (uint64_t)read_le32(desc + 0x24) << 32;
        if (!map_block || map_block >= mnt->blocks_count) return -EIO;
        uint8_t *map = malloc(mnt->block_size);
        if (!map) return -ENOMEM;
        status = read_ext4_block(mnt, map_block, map);
        if (status < 0) { free(map); return status; }
        if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
            uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), map, bitmap_bytes);
            uint32_t stored = read_le16(desc + 0x1A);
            if (mnt->desc_size >= 64) stored |= (uint32_t)read_le16(desc + 0x3A) << 16;
            else checksum &= 0xFFFF;
            if (stored != checksum) { free(map); return -EIO; }
        }
        uint64_t group_base = (uint64_t)current * mnt->inodes_per_group;
        if (group_base >= mnt->inodes_count) { free(map); return -EIO; }
        uint64_t remaining = mnt->inodes_count - group_base;
        uint32_t valid = remaining < mnt->inodes_per_group ? (uint32_t)remaining : mnt->inodes_per_group;
        uint32_t free_count = 0;
        for (uint32_t bit = 0; bit < valid; bit++) if (!(map[bit >> 3] & (1U << (bit & 7U)))) free_count++;
        for (uint32_t bit = valid; bit < mnt->inodes_per_group; bit++) if (!(map[bit >> 3] & (1U << (bit & 7U)))) { free(map); return -EIO; }
        uint32_t stored_free = read_le16(desc + 0x0E);
        if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) stored_free |= (uint32_t)read_le16(desc + 0x2E) << 16;
        if (stored_free != free_count) { free(map); return -EIO; }
        if (!free_count) { free(map); continue; }
        uint32_t bit = 0;
        while (map[bit >> 3] & (1U << (bit & 7U))) bit++;
        map[bit >> 3] |= 1U << (bit & 7U);
        free_count--;
        if (mnt->feature_ro_compat & (EXT4_FEATURE_RO_COMPAT_METADATA_CSUM | EXT4_FEATURE_RO_COMPAT_GDT_CSUM)) {
            uint32_t itable_unused = read_le16(desc + 0x1C);
            if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) itable_unused |= (uint32_t)read_le16(desc + 0x32) << 16;
            if (bit >= mnt->inodes_per_group - itable_unused) itable_unused = mnt->inodes_per_group - bit - 1;
            write_le16(desc + 0x1C, (uint16_t)itable_unused);
            if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) write_le16(desc + 0x32, (uint16_t)(itable_unused >> 16));
        }
        write_le16(desc + 0x0E, (uint16_t)free_count);
        if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) write_le16(desc + 0x2E, (uint16_t)(free_count >> 16));
        if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
            uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), map, bitmap_bytes);
            write_le16(desc + 0x1A, (uint16_t)checksum);
            if (mnt->desc_size >= 64) write_le16(desc + 0x3A, (uint16_t)(checksum >> 16));
        }
        update_group_checksum(mnt, current, desc);
        *ino = (uint32_t)(group_base + bit + 1);
        *bitmap = map;
        *bitmap_block = map_block;
        *group = current;
        return 0;
    }
    return -ENOSPC;
}

static int commit_ext4_new_file(ext4_mount_t *mnt, uint8_t desc[64], uint8_t *inode_map, uint64_t inode_map_block, uint32_t inode_group, uint32_t new_ino, const uint8_t *new_raw, uint64_t dir_block, const uint8_t *dir_data, const ext4_inode_location_t *parent_location, const uint8_t *parent_raw) {
    uint8_t super[EXT4_SUPER_SIZE];
    int status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    bool committed = false;
    if (status == 0 && mnt->has_journal) {
        if (read_le32(super + 0x10) == 0) status = -ENOSPC;
        else {
            write_le32(super + 0x10, read_le32(super + 0x10) - 1);
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            ext4_journal_batch_t batch = {0};
            status = stage_superblock(mnt, &batch, super);
            if (status == 0) status = stage_full_block(mnt, &batch, inode_map_block, inode_map);
            if (status == 0) status = stage_group_descriptor(mnt, &batch, inode_group, desc);
            ext4_inode_location_t new_location;
            if (status == 0) status = locate_ext4_inode(mnt, new_ino, &new_location);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &new_location, new_raw, mnt->inode_size);
            if (status == 0) status = stage_full_block(mnt, &batch, dir_block, dir_data);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, parent_location, parent_raw, mnt->inode_size);
            if (status == 0) status = commit_staged_blocks(mnt, &batch);
            free_staged_batch(&batch);
        }
        if (status == 0) mnt->state = read_le16(super + 0x3A);
        return status;
    }
    if (status == 0) {
        if (read_le32(super + 0x10) == 0) status = -ENOSPC;
        else {
            write_le32(super + 0x10, read_le32(super + 0x10) - 1);
            write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
            if (status == 0) committed = true;
        }
    }
    if (status == 0) status = write_ext4_block(mnt, inode_map_block, inode_map);
    if (status == 0) status = write_ext4_group_desc(mnt, inode_group, desc);
    if (status == 0) {
        ext4_inode_location_t new_location;
        status = locate_ext4_inode(mnt, new_ino, &new_location);
        if (status == 0) status = write_ext4_inode_bytes(mnt, &new_location, new_raw);
    }
    if (status == 0) status = write_ext4_block(mnt, dir_block, dir_data);
    if (status == 0) status = write_ext4_inode_bytes(mnt, parent_location, parent_raw);
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    }
    if (status == 0) mnt->state = read_le16(super + 0x3A);
    else if (committed) mnt->read_only = true;
    return status;
}

int create_ext4(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    if (!path || path[0] != '/') return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)relative;
    char parent[256];
    char name[256];
    int status = split_ext4_parent(path, parent, name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *parent_relative;
    ext4_mount_t *parent_mnt = find_ext4_mount(parent, &parent_relative);
    if (parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    status = resolve_ext4_inode(mnt, parent_relative, true, &parent_ino, &parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    uint32_t existing;
    status = lookup_ext4_child(mnt, parent_ino, name, strlen(name), &existing);
    if (status == 0) { spin_unlock_irqrestore(&ext4_lock, irq); return -EEXIST; }
    if (status != -ENOENT) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    (void)existing;
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    ext4_inode_location_t parent_location;
    status = locate_ext4_inode(mnt, parent_ino, &parent_location);
    uint8_t *parent_raw = NULL;
    uint8_t *new_raw = NULL;
    uint8_t *dir_data = NULL;
    uint8_t *inode_map = NULL;
    uint64_t inode_map_block = 0;
    uint32_t inode_group = 0;
    uint32_t new_ino = 0;
    uint64_t dir_block = 0;
    uint8_t desc[64];
    if (status == 0) {
        parent_raw = malloc(mnt->inode_size);
        if (!parent_raw) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, parent_raw, mnt->inode_size, parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, parent_ino, parent_raw);
    if (status == 0 && (read_le32(parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = allocate_ext4_inode(mnt, &new_ino, desc, &inode_map, &inode_map_block, &inode_group);
    if (status == 0) {
        new_raw = malloc(mnt->inode_size);
        if (!new_raw) status = -ENOMEM;
    }
    if (status == 0) {
        uint16_t extra = 0;
        if (mnt->inode_size > EXT4_GOOD_OLD_INODE_SIZE) extra = mnt->inode_size >= 160 ? 32 : (mnt->inode_size - EXT4_GOOD_OLD_INODE_SIZE) & ~3U;
        memset(new_raw, 0, mnt->inode_size);
        write_le16(new_raw, (uint16_t)mode);
        write_le32(new_raw + 32, EXT4_EXTENTS_FL);
        write_le16(new_raw + 40, EXT4_EXTENT_MAGIC);
        write_le16(new_raw + 44, EXT4_ROOT_EXTENT_ENTRIES);
        write_le16(new_raw + 2, (uint16_t)uid);
        write_le32(new_raw + 8, now_sec);
        write_le32(new_raw + 12, now_sec);
        write_le32(new_raw + 16, now_sec);
        write_le16(new_raw + 24, (uint16_t)gid);
        write_le16(new_raw + 26, 1);
        write_le16(new_raw + 120, (uint16_t)(uid >> 16));
        write_le16(new_raw + 122, (uint16_t)(gid >> 16));
        if (extra) write_le16(new_raw + 128, extra);
        if (extra >= 12) {
            write_le32(new_raw + 132, now_extra);
            write_le32(new_raw + 136, now_extra);
            write_le32(new_raw + 144, now_sec);
            write_le32(new_raw + 148, now_extra);
        }
        if (extra >= 16) write_le32(new_raw + 140, now_extra);
        update_inode_checksum(mnt, new_ino, new_raw);
    }
    if (status == 0) {
        dir_data = malloc(mnt->block_size);
        if (!dir_data) status = -ENOMEM;
    }
    if (status == 0) status = insert_ext4_dirent(mnt, parent_raw, name, strlen(name), new_ino, EXT4_FT_REG_FILE, &dir_block, dir_data, 0, NULL);
    if (status == 0) status = finalize_ext4_dir_block(mnt, parent_ino, parent_raw, dir_data);
    if (status == 0) {
        write_le32(parent_raw + 12, now_sec);
        write_le32(parent_raw + 16, now_sec);
        uint16_t parent_extra = mnt->inode_size >= 132 ? read_le16(parent_raw + 128) : 0;
        if (parent_extra >= 12) {
            write_le32(parent_raw + 132, now_extra);
            write_le32(parent_raw + 136, now_extra);
        }
        update_inode_checksum(mnt, parent_ino, parent_raw);
    }
    if (status == 0) status = commit_ext4_new_file(mnt, desc, inode_map, inode_map_block, inode_group, new_ino, new_raw, dir_block, dir_data, &parent_location, parent_raw);
    free(inode_map);
    free(new_raw);
    free(dir_data);
    free(parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int symlink_ext4(const char *target, const char *path, uid_t uid, gid_t gid) {
    if (!target || !target[0] || !path || path[0] != '/') return -EINVAL;
    size_t target_length = strlen(target);
    if (target_length > 60) return -EOPNOTSUPP;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)relative;
    char parent[256];
    char name[256];
    int status = split_ext4_parent(path, parent, name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *parent_relative;
    ext4_mount_t *parent_mnt = find_ext4_mount(parent, &parent_relative);
    if (parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    status = resolve_ext4_inode(mnt, parent_relative, true, &parent_ino, &parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (parent_inode.flags & EXT4_IMMUTABLE_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    uint32_t existing;
    status = lookup_ext4_child(mnt, parent_ino, name, strlen(name), &existing);
    if (status == 0) { spin_unlock_irqrestore(&ext4_lock, irq); return -EEXIST; }
    if (status != -ENOENT) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    (void)existing;
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    ext4_inode_location_t parent_location;
    status = locate_ext4_inode(mnt, parent_ino, &parent_location);
    uint8_t *parent_raw = NULL;
    uint8_t *new_raw = NULL;
    uint8_t *dir_data = NULL;
    uint8_t *inode_map = NULL;
    uint64_t inode_map_block = 0;
    uint32_t inode_group = 0;
    uint32_t new_ino = 0;
    uint64_t dir_block = 0;
    uint8_t desc[64];
    if (status == 0) {
        parent_raw = malloc(mnt->inode_size);
        if (!parent_raw) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, parent_raw, mnt->inode_size, parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, parent_ino, parent_raw);
    if (status == 0 && (read_le32(parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = allocate_ext4_inode(mnt, &new_ino, desc, &inode_map, &inode_map_block, &inode_group);
    if (status == 0) {
        new_raw = malloc(mnt->inode_size);
        if (!new_raw) status = -ENOMEM;
    }
    if (status == 0) {
        uint16_t extra = 0;
        if (mnt->inode_size > EXT4_GOOD_OLD_INODE_SIZE) extra = mnt->inode_size >= 160 ? 32 : (mnt->inode_size - EXT4_GOOD_OLD_INODE_SIZE) & ~3U;
        memset(new_raw, 0, mnt->inode_size);
        write_le16(new_raw, S_IFLNK | 0777);
        write_le16(new_raw + 2, (uint16_t)uid);
        write_le32(new_raw + 4, (uint32_t)target_length);
        write_le32(new_raw + 8, now_sec);
        write_le32(new_raw + 12, now_sec);
        write_le32(new_raw + 16, now_sec);
        write_le16(new_raw + 24, (uint16_t)gid);
        write_le16(new_raw + 26, 1);
        write_le16(new_raw + 120, (uint16_t)(uid >> 16));
        write_le16(new_raw + 122, (uint16_t)(gid >> 16));
        memcpy(new_raw + 40, target, target_length);
        if (extra) write_le16(new_raw + 128, extra);
        if (extra >= 12) {
            write_le32(new_raw + 132, now_extra);
            write_le32(new_raw + 136, now_extra);
            write_le32(new_raw + 144, now_sec);
            write_le32(new_raw + 148, now_extra);
        }
        if (extra >= 16) write_le32(new_raw + 140, now_extra);
        update_inode_checksum(mnt, new_ino, new_raw);
    }
    if (status == 0) {
        dir_data = malloc(mnt->block_size);
        if (!dir_data) status = -ENOMEM;
    }
    if (status == 0) status = insert_ext4_dirent(mnt, parent_raw, name, strlen(name), new_ino, EXT4_FT_SYMLINK, &dir_block, dir_data, 0, NULL);
    if (status == 0) status = finalize_ext4_dir_block(mnt, parent_ino, parent_raw, dir_data);
    if (status == 0) {
        write_le32(parent_raw + 12, now_sec);
        write_le32(parent_raw + 16, now_sec);
        uint16_t parent_extra = mnt->inode_size >= 132 ? read_le16(parent_raw + 128) : 0;
        if (parent_extra >= 12) {
            write_le32(parent_raw + 132, now_extra);
            write_le32(parent_raw + 136, now_extra);
        }
        update_inode_checksum(mnt, parent_ino, parent_raw);
    }
    if (status == 0) status = commit_ext4_new_file(mnt, desc, inode_map, inode_map_block, inode_group, new_ino, new_raw, dir_block, dir_data, &parent_location, parent_raw);
    free(inode_map);
    free(new_raw);
    free(dir_data);
    free(parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int link_ext4(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *old_relative;
    ext4_mount_t *old_mnt = find_ext4_mount(oldpath, &old_relative);
    const char *new_relative;
    ext4_mount_t *new_mnt = find_ext4_mount(newpath, &new_relative);
    if (!old_mnt || !new_mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (old_mnt != new_mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    ext4_mount_t *mnt = old_mnt;
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)old_relative;
    (void)new_relative;
    uint32_t old_ino;
    ext4_inode_t old_inode;
    int status = resolve_ext4_inode(mnt, old_relative, false, &old_ino, &old_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((old_inode.mode & S_IFMT) == S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    if (old_inode.flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    char parent[256];
    char name[256];
    status = split_ext4_parent(newpath, parent, name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *parent_relative;
    ext4_mount_t *parent_mnt = find_ext4_mount(parent, &parent_relative);
    if (parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    status = resolve_ext4_inode(mnt, parent_relative, true, &parent_ino, &parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (parent_inode.flags & EXT4_IMMUTABLE_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    uint32_t existing;
    status = lookup_ext4_child(mnt, parent_ino, name, strlen(name), &existing);
    if (status == 0) { spin_unlock_irqrestore(&ext4_lock, irq); return -EEXIST; }
    if (status != -ENOENT) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    (void)existing;
    uint8_t file_type = EXT4_FT_UNKNOWN;
    if ((old_inode.mode & S_IFMT) == S_IFREG) file_type = EXT4_FT_REG_FILE;
    if ((old_inode.mode & S_IFMT) == S_IFLNK) file_type = EXT4_FT_SYMLINK;
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    ext4_inode_location_t parent_location;
    status = locate_ext4_inode(mnt, parent_ino, &parent_location);
    ext4_inode_location_t old_location;
    if (status == 0) status = locate_ext4_inode(mnt, old_ino, &old_location);
    uint8_t *parent_raw = NULL;
    uint8_t *old_raw = NULL;
    uint8_t *dir_data = NULL;
    uint64_t dir_block = 0;
    uint8_t super[EXT4_SUPER_SIZE];
    bool committed = false;
    if (status == 0) {
        parent_raw = malloc(mnt->inode_size);
        old_raw = malloc(mnt->inode_size);
        dir_data = malloc(mnt->block_size);
        if (!parent_raw || !old_raw || !dir_data) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, parent_raw, mnt->inode_size, parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, parent_ino, parent_raw);
    if (status == 0 && (read_le32(parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = read_checked_device(mnt, old_raw, mnt->inode_size, old_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, old_ino, old_raw);
    if (status == 0) status = insert_ext4_dirent(mnt, parent_raw, name, strlen(name), old_ino, file_type, &dir_block, dir_data, 0, NULL);
    if (status == 0) status = finalize_ext4_dir_block(mnt, parent_ino, parent_raw, dir_data);
    if (status == 0) {
        uint16_t links = read_le16(old_raw + 26);
        if (links >= 65000) status = -EMLINK;
        else {
            write_le16(old_raw + 26, links + 1);
            write_le32(old_raw + 12, now_sec);
            uint16_t old_extra = mnt->inode_size >= 132 ? read_le16(old_raw + 128) : 0;
            if (old_extra >= 12) write_le32(old_raw + 132, now_extra);
            update_inode_checksum(mnt, old_ino, old_raw);
        }
    }
    if (status == 0) {
        write_le32(parent_raw + 12, now_sec);
        write_le32(parent_raw + 16, now_sec);
        uint16_t parent_extra = mnt->inode_size >= 132 ? read_le16(parent_raw + 128) : 0;
        if (parent_extra >= 12) {
            write_le32(parent_raw + 132, now_extra);
            write_le32(parent_raw + 136, now_extra);
        }
        update_inode_checksum(mnt, parent_ino, parent_raw);
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && mnt->has_journal) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        ext4_journal_batch_t batch = {0};
        status = stage_superblock(mnt, &batch, super);
        if (status == 0) status = stage_full_block(mnt, &batch, dir_block, dir_data);
        if (status == 0) status = stage_inode_bytes(mnt, &batch, &old_location, old_raw, mnt->inode_size);
        if (status == 0) status = stage_inode_bytes(mnt, &batch, &parent_location, parent_raw, mnt->inode_size);
        if (status == 0) status = commit_staged_blocks(mnt, &batch);
        free_staged_batch(&batch);
        if (status == 0) mnt->state = read_le16(super + 0x3A);
        free(old_raw);
        free(dir_data);
        free(parent_raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    if (status == 0) status = write_ext4_block(mnt, dir_block, dir_data);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &old_location, old_raw);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &parent_location, parent_raw);
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    }
    if (status == 0) mnt->state = read_le16(super + 0x3A);
    else if (committed) mnt->read_only = true;
    free(old_raw);
    free(dir_data);
    free(parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

static int check_ext4_dir_empty(ext4_mount_t *mnt, const ext4_inode_t *dir) {
    uint8_t *block = malloc(mnt->block_size);
    if (!block) return -ENOMEM;
    int result = 0;
    for (uint64_t offset = 0; offset < dir->size && result == 0; offset += mnt->block_size) {
        uint64_t logical = offset / mnt->block_size;
        if (logical > UINT32_MAX) { result = -EFBIG; break; }
        uint64_t physical;
        int mapped = map_ext4_inode_block(mnt, dir, (uint32_t)logical, &physical);
        if (mapped < 0) { result = mapped; break; }
        if (mapped != 1) continue;
        result = read_ext4_block(mnt, physical, block);
        if (result < 0) break;
        result = 0;
        uint32_t pos = 0;
        while (pos < mnt->block_size) {
            if (pos + 8 > mnt->block_size) { result = -EIO; break; }
            uint32_t entry_ino = read_le32(block + pos);
            uint16_t rec_len = read_le16(block + pos + 4);
            uint8_t name_length = block[pos + 6];
            if (rec_len == 0 && entry_ino == 0) break;
            if (rec_len < 8 || (rec_len & 3) || rec_len > mnt->block_size - pos) { result = -EIO; break; }
            if (entry_ino) {
                bool dot = name_length == 1 && block[pos + 8] == '.';
                bool dotdot = name_length == 2 && block[pos + 8] == '.' && block[pos + 9] == '.';
                if (!dot && !dotdot) { result = -ENOTEMPTY; break; }
            }
            pos += rec_len;
        }
    }
    free(block);
    return result;
}

int mkdir_ext4(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    if (!path || path[0] != '/') return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)relative;
    char parent[256];
    char name[256];
    int status = split_ext4_parent(path, parent, name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *parent_relative;
    ext4_mount_t *parent_mnt = find_ext4_mount(parent, &parent_relative);
    if (parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    status = resolve_ext4_inode(mnt, parent_relative, true, &parent_ino, &parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (parent_inode.flags & EXT4_IMMUTABLE_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    uint32_t existing;
    status = lookup_ext4_child(mnt, parent_ino, name, strlen(name), &existing);
    if (status == 0) { spin_unlock_irqrestore(&ext4_lock, irq); return -EEXIST; }
    if (status != -ENOENT) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    (void)existing;
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    ext4_inode_location_t parent_location;
    status = locate_ext4_inode(mnt, parent_ino, &parent_location);
    uint8_t *parent_raw = NULL;
    uint8_t *new_raw = NULL;
    uint8_t *dir_data = NULL;
    uint8_t *new_dir = NULL;
    uint8_t *inode_map = NULL;
    uint64_t inode_map_block = 0;
    uint32_t inode_group = 0;
    uint32_t new_ino = 0;
    uint64_t dir_block = 0;
    uint64_t new_block = 0;
    uint8_t desc[64];
    uint8_t super[EXT4_SUPER_SIZE];
    ext4_write_context_t context = {0};
    bool committed = false;
    uint8_t file_type = (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) ? EXT4_FT_DIR : 0;
    if (status == 0) {
        parent_raw = malloc(mnt->inode_size);
        new_raw = malloc(mnt->inode_size);
        dir_data = malloc(mnt->block_size);
        new_dir = malloc(mnt->block_size);
        if (!parent_raw || !new_raw || !dir_data || !new_dir) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, parent_raw, mnt->inode_size, parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, parent_ino, parent_raw);
    if (status == 0 && (read_le32(parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = allocate_ext4_inode(mnt, &new_ino, desc, &inode_map, &inode_map_block, &inode_group);
    if (status == 0) {
        uint32_t allocated_group;
        uint32_t allocated_length;
        uint64_t allocated_start;
        status = allocate_ext4_run(mnt, &context, inode_group, 1, &allocated_start, &allocated_length, &allocated_group);
        if (status == 0 && allocated_length != 1) status = -ENOSPC;
        if (status == 0) new_block = allocated_start;
    }
    if (status == 0) {
        uint16_t extra = 0;
        if (mnt->inode_size > EXT4_GOOD_OLD_INODE_SIZE) extra = mnt->inode_size >= 160 ? 32 : (mnt->inode_size - EXT4_GOOD_OLD_INODE_SIZE) & ~3U;
        memset(new_raw, 0, mnt->inode_size);
        write_le16(new_raw, S_IFDIR | (mode & 07777));
        write_le16(new_raw + 2, (uint16_t)uid);
        write_le32(new_raw + 4, mnt->block_size);
        write_le32(new_raw + 8, now_sec);
        write_le32(new_raw + 12, now_sec);
        write_le32(new_raw + 16, now_sec);
        write_le16(new_raw + 24, (uint16_t)gid);
        write_le16(new_raw + 26, 2);
        write_le32(new_raw + 28, mnt->block_size / 512U);
        write_le32(new_raw + 32, EXT4_EXTENTS_FL);
        write_le16(new_raw + 40, EXT4_EXTENT_MAGIC);
        write_le16(new_raw + 42, 1);
        write_le16(new_raw + 44, EXT4_ROOT_EXTENT_ENTRIES);
        write_le16(new_raw + 46, 0);
        write_le32(new_raw + 52, 0);
        write_le16(new_raw + 56, 1);
        write_le16(new_raw + 58, (uint16_t)(new_block >> 32));
        write_le32(new_raw + 60, (uint32_t)new_block);
        write_le16(new_raw + 120, (uint16_t)(uid >> 16));
        write_le16(new_raw + 122, (uint16_t)(gid >> 16));
        if (extra) write_le16(new_raw + 128, extra);
        if (extra >= 12) {
            write_le32(new_raw + 132, now_extra);
            write_le32(new_raw + 136, now_extra);
            write_le32(new_raw + 144, now_sec);
            write_le32(new_raw + 148, now_extra);
        }
        if (extra >= 16) write_le32(new_raw + 140, now_extra);
        update_inode_checksum(mnt, new_ino, new_raw);
        memset(new_dir, 0, mnt->block_size);
        write_le32(new_dir, new_ino);
        write_le16(new_dir + 4, 12);
        new_dir[6] = 1;
        new_dir[7] = file_type;
        new_dir[8] = '.';
        write_le32(new_dir + 12, parent_ino);
        write_le16(new_dir + 16, mnt->block_size - 12);
        new_dir[18] = 2;
        new_dir[19] = file_type;
        new_dir[20] = '.';
        new_dir[21] = '.';
        status = finalize_ext4_dir_block(mnt, new_ino, new_raw, new_dir);
    }
    if (status == 0) status = insert_ext4_dirent(mnt, parent_raw, name, strlen(name), new_ino, EXT4_FT_DIR, &dir_block, dir_data, 0, NULL);
    if (status == 0) status = finalize_ext4_dir_block(mnt, parent_ino, parent_raw, dir_data);
    if (status == 0) {
        uint16_t parent_links = read_le16(parent_raw + 26);
        if (parent_links >= 65000) status = -EMLINK;
        else {
            write_le16(parent_raw + 26, parent_links + 1);
            write_le32(parent_raw + 12, now_sec);
            write_le32(parent_raw + 16, now_sec);
            uint16_t parent_extra = mnt->inode_size >= 132 ? read_le16(parent_raw + 128) : 0;
            if (parent_extra >= 12) {
                write_le32(parent_raw + 132, now_extra);
                write_le32(parent_raw + 136, now_extra);
            }
            update_inode_checksum(mnt, parent_ino, parent_raw);
        }
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && mnt->has_journal) {
        uint32_t free_inodes = read_le32(super + 0x10);
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (!free_inodes || !free_blocks || !mnt->free_blocks) status = -ENOSPC;
        else {
            write_le32(super + 0x10, free_inodes - 1);
            write_le32(super + 0x0C, (uint32_t)(free_blocks - 1));
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)((free_blocks - 1) >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            bool merged = merge_ext4_inode_desc(mnt, &context, inode_group, desc);
            finalize_group_states(mnt, &context);
            ext4_journal_batch_t batch = {0};
            status = stage_superblock(mnt, &batch, super);
            if (status == 0) status = stage_full_block(mnt, &batch, inode_map_block, inode_map);
            if (status == 0 && !merged) status = stage_group_descriptor(mnt, &batch, inode_group, desc);
            for (size_t i = 0; i < context.group_count && status == 0; i++) {
                status = stage_full_block(mnt, &batch, context.groups[i].bitmap_block, context.groups[i].bitmap);
                if (status == 0) status = stage_group_descriptor(mnt, &batch, context.groups[i].group, context.groups[i].descriptor);
            }
            ext4_inode_location_t new_location;
            if (status == 0) status = locate_ext4_inode(mnt, new_ino, &new_location);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &new_location, new_raw, mnt->inode_size);
            if (status == 0) status = stage_full_block(mnt, &batch, new_block, new_dir);
            if (status == 0) status = stage_full_block(mnt, &batch, dir_block, dir_data);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &parent_location, parent_raw, mnt->inode_size);
            if (status == 0) status = commit_staged_blocks(mnt, &batch);
            free_staged_batch(&batch);
        }
        if (status == 0) {
            mnt->free_blocks--;
            mnt->state = read_le16(super + 0x3A);
        }
        free_ext4_write_context(&context);
        free(inode_map);
        free(new_raw);
        free(dir_data);
        free(new_dir);
        free(parent_raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        uint32_t free_inodes = read_le32(super + 0x10);
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (!free_inodes || !free_blocks || !mnt->free_blocks) status = -ENOSPC;
        else {
            write_le32(super + 0x10, free_inodes - 1);
            write_le32(super + 0x0C, (uint32_t)(free_blocks - 1));
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)((free_blocks - 1) >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
            if (status == 0) committed = true;
        }
    }
    if (status == 0) status = write_ext4_block(mnt, inode_map_block, inode_map);
    bool merged = merge_ext4_inode_desc(mnt, &context, inode_group, desc);
    if (status == 0 && !merged) status = write_ext4_group_desc(mnt, inode_group, desc);
    if (status == 0) status = commit_ext4_group_states(mnt, &context);
    if (status == 0) {
        ext4_inode_location_t new_location;
        status = locate_ext4_inode(mnt, new_ino, &new_location);
        if (status == 0) status = write_ext4_inode_bytes(mnt, &new_location, new_raw);
    }
    if (status == 0) status = write_ext4_block(mnt, new_block, new_dir);
    if (status == 0) status = write_ext4_block(mnt, dir_block, dir_data);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &parent_location, parent_raw);
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    }
    if (status == 0) {
        mnt->free_blocks--;
        mnt->state = read_le16(super + 0x3A);
    } else if (committed) mnt->read_only = true;
    free_ext4_write_context(&context);
    free(inode_map);
    free(new_raw);
    free(dir_data);
    free(new_dir);
    free(parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int rename_ext4(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *old_relative;
    ext4_mount_t *old_mnt = find_ext4_mount(oldpath, &old_relative);
    const char *new_relative;
    ext4_mount_t *new_mnt = find_ext4_mount(newpath, &new_relative);
    if (!old_mnt || !new_mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (old_mnt != new_mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    ext4_mount_t *mnt = old_mnt;
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)old_relative;
    (void)new_relative;
    uint32_t old_ino;
    ext4_inode_t old_inode;
    int status = resolve_ext4_inode(mnt, old_relative, false, &old_ino, &old_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if (old_inode.flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    bool old_is_dir = (old_inode.mode & S_IFMT) == S_IFDIR;
    char old_parent_path[256];
    char old_name[256];
    status = split_ext4_parent(oldpath, old_parent_path, old_name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    char new_parent_path[256];
    char new_name[256];
    status = split_ext4_parent(newpath, new_parent_path, new_name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *old_parent_relative;
    ext4_mount_t *old_parent_mnt = find_ext4_mount(old_parent_path, &old_parent_relative);
    const char *new_parent_relative;
    ext4_mount_t *new_parent_mnt = find_ext4_mount(new_parent_path, &new_parent_relative);
    if (old_parent_mnt != mnt || new_parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t old_parent_ino;
    ext4_inode_t old_parent_inode;
    status = resolve_ext4_inode(mnt, old_parent_relative, true, &old_parent_ino, &old_parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    uint32_t new_parent_ino;
    ext4_inode_t new_parent_inode;
    status = resolve_ext4_inode(mnt, new_parent_relative, true, &new_parent_ino, &new_parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((old_parent_inode.mode & S_IFMT) != S_IFDIR || (new_parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if ((old_parent_inode.flags & EXT4_IMMUTABLE_FL) || (new_parent_inode.flags & EXT4_IMMUTABLE_FL)) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    uint32_t new_ino = 0;
    ext4_inode_t new_inode;
    bool new_exists = false;
    status = resolve_ext4_inode(mnt, new_relative, false, &new_ino, &new_inode);
    if (status == 0) new_exists = true;
    else if (status != -ENOENT) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    else status = 0;
    bool new_is_dir = new_exists && (new_inode.mode & S_IFMT) == S_IFDIR;
    if (new_exists && old_is_dir && !new_is_dir) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (new_exists && !old_is_dir && new_is_dir) { spin_unlock_irqrestore(&ext4_lock, irq); return -EISDIR; }
    if (new_exists && (new_inode.flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL))) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    if (new_exists && new_is_dir) {
        status = check_ext4_dir_empty(mnt, &new_inode);
        if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    }
    if (new_exists && new_ino == old_ino) { spin_unlock_irqrestore(&ext4_lock, irq); return 0; }
    if (old_is_dir) {
        uint32_t ancestor = new_parent_ino;
        for (int depth = 0; depth < 32; depth++) {
            if (ancestor == old_ino) { spin_unlock_irqrestore(&ext4_lock, irq); return -EINVAL; }
            if (ancestor == EXT4_ROOT_INO) break;
            uint32_t next;
            status = lookup_ext4_child(mnt, ancestor, "..", 2, &next);
            if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status == -ENOENT ? -EIO : status; }
            if (next == ancestor) break;
            ancestor = next;
        }
    }
    uint8_t file_type = EXT4_FT_UNKNOWN;
    if (!old_is_dir && (old_inode.mode & S_IFMT) == S_IFREG) file_type = EXT4_FT_REG_FILE;
    if (!old_is_dir && (old_inode.mode & S_IFMT) == S_IFLNK) file_type = EXT4_FT_SYMLINK;
    if (old_is_dir) file_type = EXT4_FT_DIR;
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    bool same_parent = old_parent_ino == new_parent_ino;
    ext4_inode_location_t old_parent_location;
    status = locate_ext4_inode(mnt, old_parent_ino, &old_parent_location);
    ext4_inode_location_t new_parent_location = old_parent_location;
    if (status == 0 && !same_parent) status = locate_ext4_inode(mnt, new_parent_ino, &new_parent_location);
    ext4_inode_location_t old_location;
    if (status == 0) status = locate_ext4_inode(mnt, old_ino, &old_location);
    ext4_inode_location_t new_location;
    if (status == 0 && new_exists) status = locate_ext4_inode(mnt, new_ino, &new_location);
    uint8_t *old_parent_raw = NULL;
    uint8_t *new_parent_raw = NULL;
    uint8_t *old_raw = NULL;
    uint8_t *new_raw = NULL;
    uint8_t *old_dir_data = NULL;
    uint8_t *new_dir_data = NULL;
    uint8_t *dot_data = NULL;
    uint64_t old_dir_block = 0;
    uint64_t new_dir_block = 0;
    uint64_t dot_block = 0;
    uint8_t *inode_map = NULL;
    uint64_t inode_map_block = 0;
    uint32_t inode_group = 0;
    uint8_t desc[64];
    uint8_t super[EXT4_SUPER_SIZE];
    ext4_write_context_t context = {0};
    uint32_t freed = 0;
    bool committed = false;
    bool free_replaced = false;
    if (status == 0) {
        old_parent_raw = malloc(mnt->inode_size);
        new_parent_raw = same_parent ? old_parent_raw : malloc(mnt->inode_size);
        old_raw = malloc(mnt->inode_size);
        old_dir_data = malloc(mnt->block_size);
        new_dir_data = malloc(mnt->block_size);
        dot_data = malloc(mnt->block_size);
        if (new_exists) new_raw = malloc(mnt->inode_size);
        if (!old_parent_raw || (!same_parent && !new_parent_raw) || !old_raw || !old_dir_data || !new_dir_data || !dot_data || (new_exists && !new_raw)) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, old_parent_raw, mnt->inode_size, old_parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, old_parent_ino, old_parent_raw);
    if (status == 0 && (read_le32(old_parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0 && !same_parent) status = read_checked_device(mnt, new_parent_raw, mnt->inode_size, new_parent_location.byte_offset);
    if (status == 0 && !same_parent) status = validate_inode_checksum(mnt, new_parent_ino, new_parent_raw);
    if (status == 0 && !same_parent && (read_le32(new_parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = read_checked_device(mnt, old_raw, mnt->inode_size, old_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, old_ino, old_raw);
    if (status == 0 && new_exists) status = read_checked_device(mnt, new_raw, mnt->inode_size, new_location.byte_offset);
    if (status == 0 && new_exists) status = validate_inode_checksum(mnt, new_ino, new_raw);
    if (status == 0) status = remove_ext4_dirent(mnt, old_parent_raw, old_name, strlen(old_name), &old_dir_block, old_dir_data);
    if (status == 0 && old_is_dir) {
        uint16_t links = read_le16(old_parent_raw + 26);
        if (!links) status = -EIO;
        else write_le16(old_parent_raw + 26, links - 1);
    }
    if (status == 0 && new_exists) {
        status = remove_ext4_dirent(mnt, new_parent_raw, new_name, strlen(new_name), &new_dir_block, new_dir_data);
        if (status == 0 && new_is_dir) {
            uint16_t links = read_le16(new_parent_raw + 26);
            if (!links) status = -EIO;
            else write_le16(new_parent_raw + 26, links - 1);
        }
        if (status == 0) {
            uint16_t links = read_le16(new_raw + 26);
            if (!links) status = -EIO;
            else {
                links--;
                write_le16(new_raw + 26, links);
                write_le32(new_raw + 12, now_sec);
                uint16_t new_extra = mnt->inode_size >= 132 ? read_le16(new_raw + 128) : 0;
                if (new_extra >= 12) write_le32(new_raw + 132, now_extra);
                if (!links) {
                    free_replaced = true;
                    write_le32(new_raw + 20, now_sec);
                    write_le32(new_raw + 4, 0);
                    write_le32(new_raw + 108, 0);
                    write_le32(new_raw + 28, 0);
                    if (new_is_dir) {
                        if (read_le16(new_raw + 46) != 0 && (read_le32(new_raw + 32) & EXT4_EXTENTS_FL)) status = -EOPNOTSUPP;
                        else if (read_le32(new_raw + 32) & EXT4_EXTENTS_FL) status = truncate_ext4_extents(mnt, new_raw, 0, &context, &freed);
                        else status = free_ext4_legacy_tree(mnt, new_raw, &context, &freed);
                    } else {
                        if (read_le32(new_raw + 32) & EXT4_EXTENTS_FL) {
                            if (read_le16(new_raw + 46) != 0) status = -EOPNOTSUPP;
                            else status = truncate_ext4_extents(mnt, new_raw, 0, &context, &freed);
                        } else status = free_ext4_legacy_tree(mnt, new_raw, &context, &freed);
                    }
                }
                update_inode_checksum(mnt, new_ino, new_raw);
            }
        }
    }
    if (status == 0 && free_replaced) status = free_ext4_inode_bit(mnt, super, new_ino, desc, &inode_map, &inode_map_block, &inode_group);
    if (status == 0) status = insert_ext4_dirent(mnt, new_parent_raw, new_name, strlen(new_name), old_ino, file_type, &new_dir_block, new_dir_data, old_dir_block, old_dir_data);
    if (status == 0 && new_dir_block != old_dir_block) status = finalize_ext4_dir_block(mnt, old_parent_ino, old_parent_raw, old_dir_data);
    if (status == 0) status = finalize_ext4_dir_block(mnt, new_parent_ino, new_parent_raw, new_dir_data);
    if (status == 0 && old_is_dir) {
        uint16_t links = read_le16(new_parent_raw + 26);
        if (links >= 65000) status = -EMLINK;
        else write_le16(new_parent_raw + 26, links + 1);
    }
    if (status == 0 && old_is_dir && !same_parent) {
        uint64_t dot_physical;
        int mapped = map_ext4_parent_block(mnt, old_raw, 0, &dot_physical);
        if (mapped < 0) status = mapped;
        else if (!mapped) status = -EIO;
        else {
            status = read_ext4_block(mnt, dot_physical, dot_data);
            if (status == 0) {
                write_le32(dot_data + 12, new_parent_ino);
                dot_block = dot_physical;
            }
        }
    }
    if (status == 0) {
        write_le32(old_raw + 12, now_sec);
        update_inode_checksum(mnt, old_ino, old_raw);
        write_le32(old_parent_raw + 12, now_sec);
        write_le32(old_parent_raw + 16, now_sec);
        uint16_t old_parent_extra = mnt->inode_size >= 132 ? read_le16(old_parent_raw + 128) : 0;
        if (old_parent_extra >= 12) {
            write_le32(old_parent_raw + 132, now_extra);
            write_le32(old_parent_raw + 136, now_extra);
        }
        update_inode_checksum(mnt, old_parent_ino, old_parent_raw);
        if (!same_parent) {
            write_le32(new_parent_raw + 12, now_sec);
            write_le32(new_parent_raw + 16, now_sec);
            uint16_t new_parent_extra = mnt->inode_size >= 132 ? read_le16(new_parent_raw + 128) : 0;
            if (new_parent_extra >= 12) {
                write_le32(new_parent_raw + 132, now_extra);
                write_le32(new_parent_raw + 136, now_extra);
            }
            update_inode_checksum(mnt, new_parent_ino, new_parent_raw);
        }
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && mnt->has_journal) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            bool merged = free_replaced && merge_ext4_inode_desc(mnt, &context, inode_group, desc);
            finalize_group_states(mnt, &context);
            ext4_journal_batch_t batch = {0};
            status = stage_superblock(mnt, &batch, super);
            for (size_t i = 0; i < context.group_count && status == 0; i++) {
                status = stage_full_block(mnt, &batch, context.groups[i].bitmap_block, context.groups[i].bitmap);
                if (status == 0) status = stage_group_descriptor(mnt, &batch, context.groups[i].group, context.groups[i].descriptor);
            }
            if (status == 0 && free_replaced) status = stage_full_block(mnt, &batch, inode_map_block, inode_map);
            if (status == 0 && free_replaced && !merged) status = stage_group_descriptor(mnt, &batch, inode_group, desc);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &old_location, old_raw, mnt->inode_size);
            if (status == 0 && new_exists) status = stage_inode_bytes(mnt, &batch, &new_location, new_raw, mnt->inode_size);
            if (status == 0) status = stage_full_block(mnt, &batch, old_dir_block, old_dir_data);
            if (status == 0 && new_dir_block != old_dir_block) status = stage_full_block(mnt, &batch, new_dir_block, new_dir_data);
            if (status == 0 && dot_block) status = stage_full_block(mnt, &batch, dot_block, dot_data);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &old_parent_location, old_parent_raw, mnt->inode_size);
            if (status == 0 && !same_parent) status = stage_inode_bytes(mnt, &batch, &new_parent_location, new_parent_raw, mnt->inode_size);
            if (status == 0) status = commit_staged_blocks(mnt, &batch);
            free_staged_batch(&batch);
        }
        if (status == 0) {
            mnt->free_blocks += freed;
            mnt->state = read_le16(super + 0x3A);
        }
        free_ext4_write_context(&context);
        free(inode_map);
        free(old_raw);
        free(new_raw);
        free(old_dir_data);
        free(new_dir_data);
        free(dot_data);
        free(old_parent_raw);
        if (!same_parent) free(new_parent_raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    bool merged = free_replaced && merge_ext4_inode_desc(mnt, &context, inode_group, desc);
    if (status == 0) status = commit_ext4_group_states(mnt, &context);
    if (status == 0 && free_replaced) status = write_ext4_block(mnt, inode_map_block, inode_map);
    if (status == 0 && free_replaced && !merged) status = write_ext4_group_desc(mnt, inode_group, desc);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &old_location, old_raw);
    if (status == 0 && new_exists) status = write_ext4_inode_bytes(mnt, &new_location, new_raw);
    if (status == 0 && new_dir_block != old_dir_block) status = write_ext4_block(mnt, old_dir_block, old_dir_data);
    if (status == 0) status = write_ext4_block(mnt, new_dir_block, new_dir_data);
    if (status == 0 && dot_block) status = write_ext4_block(mnt, dot_block, dot_data);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &old_parent_location, old_parent_raw);
    if (status == 0 && !same_parent) status = write_ext4_inode_bytes(mnt, &new_parent_location, new_parent_raw);
    if (status == 0) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        }
    }
    if (status == 0) {
        mnt->free_blocks += freed;
        mnt->state = read_le16(super + 0x3A);
    } else if (committed) mnt->read_only = true;
    free_ext4_write_context(&context);
    free(inode_map);
    free(old_raw);
    free(new_raw);
    free(old_dir_data);
    free(new_dir_data);
    free(dot_data);
    free(old_parent_raw);
    if (!same_parent) free(new_parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int rmdir_ext4(const char *path) {
    if (!path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)relative;
    char parent[256];
    char name[256];
    int status = split_ext4_parent(path, parent, name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *parent_relative;
    ext4_mount_t *parent_mnt = find_ext4_mount(parent, &parent_relative);
    if (parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    status = resolve_ext4_inode(mnt, parent_relative, true, &parent_ino, &parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (parent_inode.flags & EXT4_IMMUTABLE_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    uint32_t child_ino;
    ext4_inode_t child_inode;
    status = resolve_ext4_inode(mnt, relative, false, &child_ino, &child_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((child_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (child_inode.flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    if (child_ino == EXT4_ROOT_INO) { spin_unlock_irqrestore(&ext4_lock, irq); return -EBUSY; }
    status = check_ext4_dir_empty(mnt, &child_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    ext4_inode_location_t parent_location;
    status = locate_ext4_inode(mnt, parent_ino, &parent_location);
    ext4_inode_location_t child_location;
    if (status == 0) status = locate_ext4_inode(mnt, child_ino, &child_location);
    uint8_t *parent_raw = NULL;
    uint8_t *child_raw = NULL;
    uint8_t *dir_data = NULL;
    uint64_t dir_block = 0;
    uint8_t *inode_map = NULL;
    uint64_t inode_map_block = 0;
    uint32_t inode_group = 0;
    uint8_t desc[64];
    uint8_t super[EXT4_SUPER_SIZE];
    ext4_write_context_t context = {0};
    uint32_t freed = 0;
    bool committed = false;
    bool delete_inode = false;
    if (status == 0) {
        parent_raw = malloc(mnt->inode_size);
        child_raw = malloc(mnt->inode_size);
        dir_data = malloc(mnt->block_size);
        if (!parent_raw || !child_raw || !dir_data) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, parent_raw, mnt->inode_size, parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, parent_ino, parent_raw);
    if (status == 0 && (read_le32(parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = read_checked_device(mnt, child_raw, mnt->inode_size, child_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, child_ino, child_raw);
    if (status == 0) status = remove_ext4_dirent(mnt, parent_raw, name, strlen(name), &dir_block, dir_data);
    if (status == 0) {
        uint16_t parent_links = read_le16(parent_raw + 26);
        if (!parent_links) status = -EIO;
        else {
            write_le16(parent_raw + 26, parent_links - 1);
            write_le32(parent_raw + 12, now_sec);
            write_le32(parent_raw + 16, now_sec);
            uint16_t parent_extra = mnt->inode_size >= 132 ? read_le16(parent_raw + 128) : 0;
            if (parent_extra >= 12) {
                write_le32(parent_raw + 132, now_extra);
                write_le32(parent_raw + 136, now_extra);
            }
            update_inode_checksum(mnt, parent_ino, parent_raw);
        }
    }
    if (status == 0) {
        write_le32(child_raw + 26, 0);
        write_le32(child_raw + 12, now_sec);
        write_le32(child_raw + 20, now_sec);
        uint16_t child_extra = mnt->inode_size >= 132 ? read_le16(child_raw + 128) : 0;
        if (child_extra >= 12) write_le32(child_raw + 132, now_extra);
        write_le32(child_raw + 4, 0);
        write_le32(child_raw + 108, 0);
        write_le32(child_raw + 28, 0);
        if (child_inode.flags & EXT4_EXTENTS_FL) {
            if (read_le16(child_raw + 46) != 0) status = -EOPNOTSUPP;
            else status = truncate_ext4_extents(mnt, child_raw, 0, &context, &freed);
        } else status = free_ext4_legacy_tree(mnt, child_raw, &context, &freed);
        if (status == 0) {
            delete_inode = true;
            update_inode_checksum(mnt, child_ino, child_raw);
        }
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && delete_inode) status = free_ext4_inode_bit(mnt, super, child_ino, desc, &inode_map, &inode_map_block, &inode_group);
    if (status == 0 && mnt->has_journal) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            bool merged = delete_inode && merge_ext4_inode_desc(mnt, &context, inode_group, desc);
            finalize_group_states(mnt, &context);
            ext4_journal_batch_t batch = {0};
            status = stage_superblock(mnt, &batch, super);
            for (size_t i = 0; i < context.group_count && status == 0; i++) {
                status = stage_full_block(mnt, &batch, context.groups[i].bitmap_block, context.groups[i].bitmap);
                if (status == 0) status = stage_group_descriptor(mnt, &batch, context.groups[i].group, context.groups[i].descriptor);
            }
            if (status == 0 && delete_inode) status = stage_full_block(mnt, &batch, inode_map_block, inode_map);
            if (status == 0 && delete_inode && !merged) status = stage_group_descriptor(mnt, &batch, inode_group, desc);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &child_location, child_raw, mnt->inode_size);
            if (status == 0) status = stage_full_block(mnt, &batch, dir_block, dir_data);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &parent_location, parent_raw, mnt->inode_size);
            if (status == 0) status = commit_staged_blocks(mnt, &batch);
            free_staged_batch(&batch);
        }
        if (status == 0) {
            mnt->free_blocks += freed;
            mnt->state = read_le16(super + 0x3A);
        }
        free_ext4_write_context(&context);
        free(inode_map);
        free(child_raw);
        free(dir_data);
        free(parent_raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    bool merged = delete_inode && merge_ext4_inode_desc(mnt, &context, inode_group, desc);
    if (status == 0) status = commit_ext4_group_states(mnt, &context);
    if (status == 0 && delete_inode) status = write_ext4_block(mnt, inode_map_block, inode_map);
    if (status == 0 && delete_inode && !merged) status = write_ext4_group_desc(mnt, inode_group, desc);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &child_location, child_raw);
    if (status == 0) status = write_ext4_block(mnt, dir_block, dir_data);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &parent_location, parent_raw);
    if (status == 0) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        }
    }
    if (status == 0) {
        mnt->free_blocks += freed;
        mnt->state = read_le16(super + 0x3A);
    } else if (committed) mnt->read_only = true;
    free_ext4_write_context(&context);
    free(inode_map);
    free(child_raw);
    free(dir_data);
    free(parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int set_ext4_times(const char *path, struct timespec atime, bool set_atime, struct timespec mtime, bool set_mtime, bool follow) {
    if (!path) return -EINVAL;
    if (!set_atime && !set_mtime) return 0;
    if (set_atime && (atime.tv_sec < 0 || atime.tv_nsec < 0 || atime.tv_nsec >= 1000000000L)) return -EINVAL;
    if (set_mtime && (mtime.tv_sec < 0 || mtime.tv_nsec < 0 || mtime.tv_nsec >= 1000000000L)) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, follow, &ino, &inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    (void)inode;
    ext4_inode_location_t location;
    status = locate_ext4_inode(mnt, ino, &location);
    uint8_t *raw = NULL;
    uint8_t super[EXT4_SUPER_SIZE];
    bool committed = false;
    if (status == 0) {
        raw = malloc(mnt->inode_size);
        if (!raw) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, raw, mnt->inode_size, location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, ino, raw);
    if (status == 0) {
        struct timespec now = time_get_realtime_ts();
        uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
        uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
        uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
        uint16_t extra = mnt->inode_size >= 132 ? read_le16(raw + 128) : 0;
        if (set_atime) {
            write_le32(raw + 8, (uint32_t)atime.tv_sec);
            if (extra >= 16) write_le32(raw + 140, ((uint32_t)(((uint64_t)atime.tv_sec >> 32) & 3U)) | ((uint32_t)atime.tv_nsec << 2));
        }
        if (set_mtime) {
            write_le32(raw + 16, (uint32_t)mtime.tv_sec);
            if (extra >= 12) write_le32(raw + 136, ((uint32_t)(((uint64_t)mtime.tv_sec >> 32) & 3U)) | ((uint32_t)mtime.tv_nsec << 2));
        }
        write_le32(raw + 12, now_sec);
        if (extra >= 12) write_le32(raw + 132, now_extra);
        update_inode_checksum(mnt, ino, raw);
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && mnt->has_journal) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        ext4_journal_batch_t batch = {0};
        status = stage_superblock(mnt, &batch, super);
        if (status == 0) status = stage_inode_bytes(mnt, &batch, &location, raw, mnt->inode_size);
        if (status == 0) status = commit_staged_blocks(mnt, &batch);
        free_staged_batch(&batch);
        if (status == 0) mnt->state = read_le16(super + 0x3A);
        free(raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    if (status == 0) status = write_ext4_inode_bytes(mnt, &location, raw);
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    }
    if (status == 0) mnt->state = read_le16(super + 0x3A);
    else if (committed) mnt->read_only = true;
    free(raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

static int commit_ext4_inode(ext4_mount_t *mnt, const ext4_inode_location_t *location, const uint8_t *raw) {
    uint8_t super[EXT4_SUPER_SIZE];
    int status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && mnt->has_journal) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        ext4_journal_batch_t batch = {0};
        status = stage_superblock(mnt, &batch, super);
        if (status == 0) status = stage_inode_bytes(mnt, &batch, location, raw, mnt->inode_size);
        if (status == 0) status = commit_staged_blocks(mnt, &batch);
        free_staged_batch(&batch);
        if (status == 0) mnt->state = read_le16(super + 0x3A);
        return status;
    }
    bool committed = false;
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    if (status == 0) status = write_ext4_inode_bytes(mnt, location, raw);
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    }
    if (status == 0) mnt->state = read_le16(super + 0x3A);
    else if (committed) mnt->read_only = true;
    return status;
}

int chmod_ext4(const char *path, mode_t mode, bool follow) {
    if (!path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, follow, &ino, &inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    ext4_inode_location_t location;
    status = locate_ext4_inode(mnt, ino, &location);
    uint8_t *raw = NULL;
    if (status == 0) {
        raw = malloc(mnt->inode_size);
        if (!raw) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, raw, mnt->inode_size, location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, ino, raw);
    if (status == 0) {
        struct timespec now = time_get_realtime_ts();
        uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
        uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
        uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
        write_le16(raw, (read_le16(raw) & S_IFMT) | (mode & 07777));
        write_le32(raw + 12, now_sec);
        uint16_t extra = mnt->inode_size >= 132 ? read_le16(raw + 128) : 0;
        if (extra >= 12) write_le32(raw + 132, now_extra);
        update_inode_checksum(mnt, ino, raw);
    }
    if (status == 0) status = commit_ext4_inode(mnt, &location, raw);
    free(raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int chown_ext4(const char *path, uid_t uid, gid_t gid, bool follow) {
    if (!path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, follow, &ino, &inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    (void)inode;
    ext4_inode_location_t location;
    status = locate_ext4_inode(mnt, ino, &location);
    uint8_t *raw = NULL;
    if (status == 0) {
        raw = malloc(mnt->inode_size);
        if (!raw) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, raw, mnt->inode_size, location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, ino, raw);
    if (status == 0) {
        struct timespec now = time_get_realtime_ts();
        uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
        uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
        uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
        if (uid != (uid_t)-1) {
            write_le16(raw + 2, (uint16_t)uid);
            write_le16(raw + 120, (uint16_t)(uid >> 16));
        }
        if (gid != (gid_t)-1) {
            write_le16(raw + 24, (uint16_t)gid);
            write_le16(raw + 122, (uint16_t)(gid >> 16));
        }
        write_le16(raw, read_le16(raw) & (uint16_t)~06000);
        write_le32(raw + 12, now_sec);
        uint16_t extra = mnt->inode_size >= 132 ? read_le16(raw + 128) : 0;
        if (extra >= 12) write_le32(raw + 132, now_extra);
        update_inode_checksum(mnt, ino, raw);
    }
    if (status == 0) status = commit_ext4_inode(mnt, &location, raw);
    free(raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

static int clear_ext4_blocks(ext4_mount_t *mnt, ext4_write_context_t *context, uint64_t start, uint32_t count, uint32_t *freed) {
    if (!count) return 0;
    if (start < mnt->first_data_block || start >= mnt->blocks_count || count > mnt->blocks_count - start) return -EIO;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t relative = start + i - mnt->first_data_block;
        uint32_t group = (uint32_t)(relative / mnt->blocks_per_group);
        uint32_t bit = (uint32_t)(relative % mnt->blocks_per_group);
        int status = load_ext4_group(mnt, context, group);
        if (status < 0) return status;
        ext4_group_state_t *state = NULL;
        for (size_t j = 0; j < context->group_count; j++) if (context->groups[j].group == group) state = &context->groups[j];
        if (!state) return -EIO;
        if (!check_bitmap_bit(state->bitmap, bit)) return -EIO;
        state->bitmap[bit >> 3] &= ~(1U << (bit & 7U));
        state->free_blocks++;
        (*freed)++;
    }
    return 0;
}

static bool merge_ext4_inode_desc(const ext4_mount_t *mnt, ext4_write_context_t *context, uint32_t inode_group, const uint8_t desc[64]) {
    // Fold the inode-side fields of a separately updated descriptor into the
    // matching block-side copy so a single merged descriptor is committed.
    for (size_t i = 0; i < context->group_count; i++) {
        if (context->groups[i].group != inode_group) continue;
        uint8_t *target = context->groups[i].descriptor;
        write_le16(target + 0x0E, read_le16(desc + 0x0E));
        write_le16(target + 0x1A, read_le16(desc + 0x1A));
        write_le16(target + 0x1C, read_le16(desc + 0x1C));
        if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) {
            write_le16(target + 0x2E, read_le16(desc + 0x2E));
            write_le16(target + 0x3A, read_le16(desc + 0x3A));
            write_le16(target + 0x32, read_le16(desc + 0x32));
        }
        return true;
    }
    return false;
}

static void finalize_group_states(ext4_mount_t *mnt, const ext4_write_context_t *context) {
    uint32_t bitmap_bytes = mnt->blocks_per_group >> 3;
    for (size_t i = 0; i < context->group_count; i++) {
        ext4_group_state_t *group = &context->groups[i];
        if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
            uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), group->bitmap, bitmap_bytes);
            write_le16(group->descriptor + 0x18, (uint16_t)checksum);
            if (mnt->desc_size >= 64) write_le16(group->descriptor + 0x38, (uint16_t)(checksum >> 16));
        }
        write_le16(group->descriptor + 0x0C, group->free_blocks);
        if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) write_le16(group->descriptor + 0x2C, 0);
        update_group_checksum(mnt, group->group, group->descriptor);
    }
}

static int commit_ext4_group_states(ext4_mount_t *mnt, const ext4_write_context_t *context) {
    finalize_group_states(mnt, context);
    for (size_t i = 0; i < context->group_count; i++) {
        ext4_group_state_t *group = &context->groups[i];
        int status = write_ext4_block(mnt, group->bitmap_block, group->bitmap);
        if (status < 0) return status;
        status = write_ext4_group_desc(mnt, group->group, group->descriptor);
        if (status < 0) return status;
    }
    return 0;
}

static int truncate_ext4_extents(ext4_mount_t *mnt, uint8_t *raw, uint32_t cutoff, ext4_write_context_t *context, uint32_t *freed) {
    uint8_t kept[60];
    memcpy(kept, raw + 40, 12);
    uint16_t kept_entries = 0;
    uint16_t entries = read_le16(raw + 42);
    uint16_t maximum = read_le16(raw + 44);
    if (entries > maximum || maximum > EXT4_ROOT_EXTENT_ENTRIES) return -EIO;
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *entry = raw + 52U + (size_t)i * 12U;
        uint32_t first = read_le32(entry);
        uint16_t raw_length = read_le16(entry + 4);
        bool written = raw_length <= EXT4_MAX_WRITTEN_EXTENT;
        uint32_t length = written ? raw_length : (uint32_t)raw_length - EXT4_MAX_WRITTEN_EXTENT;
        if (!length || (uint64_t)first + length > EXT4_MAX_LOGICAL_BLOCK + 1ULL) return -EIO;
        uint64_t start = (uint64_t)read_le32(entry + 8) | ((uint64_t)read_le16(entry + 6) << 32);
        if (start >= mnt->blocks_count || length > mnt->blocks_count - start) return -EIO;
        if (first + length <= cutoff) {
            memcpy(kept + 12U + (size_t)kept_entries * 12U, entry, 12);
            kept_entries++;
            continue;
        }
        if (!written) return -EOPNOTSUPP;
        if (first < cutoff) {
            uint32_t kept_length = cutoff - first;
            uint8_t *kept_entry = kept + 12U + (size_t)kept_entries * 12U;
            memcpy(kept_entry, entry, 12);
            write_le16(kept_entry + 4, (uint16_t)kept_length);
            kept_entries++;
            int status = clear_ext4_blocks(mnt, context, start + kept_length, length - kept_length, freed);
            if (status < 0) return status;
        } else {
            int status = clear_ext4_blocks(mnt, context, start, length, freed);
            if (status < 0) return status;
        }
    }
    write_le16(kept + 2, kept_entries);
    memcpy(raw + 40, kept, 60);
    return 0;
}

static int truncate_ext4_legacy(ext4_mount_t *mnt, uint8_t *raw, uint32_t cutoff, ext4_write_context_t *context, uint32_t *freed) {
    for (uint32_t i = 0; i < 12; i++) {
        if (i < cutoff) continue;
        uint32_t block = read_le32(raw + 40 + i * 4U);
        if (block) {
            int status = clear_ext4_blocks(mnt, context, block, 1, freed);
            if (status < 0) return status;
            write_le32(raw + 40 + i * 4U, 0);
        }
    }
    uint32_t ptrs = mnt->block_size / 4U;
    uint64_t single_end = 12ULL + ptrs;
    uint64_t double_end = single_end + (uint64_t)ptrs * ptrs;
    uint64_t triple_end = double_end + (uint64_t)ptrs * ptrs * ptrs;
    if (read_le32(raw + 88) && cutoff < single_end) return -EOPNOTSUPP;
    if (read_le32(raw + 92) && cutoff < double_end) return -EOPNOTSUPP;
    if (read_le32(raw + 96) && cutoff < triple_end) return -EOPNOTSUPP;
    return 0;
}

static int free_ext4_indirect(ext4_mount_t *mnt, uint64_t block, uint32_t depth, ext4_write_context_t *context, uint32_t *freed) {
    uint8_t *pointers = malloc(mnt->block_size);
    if (!pointers) return -ENOMEM;
    int status = read_ext4_block(mnt, block, pointers);
    uint32_t ptrs = mnt->block_size / 4U;
    for (uint32_t i = 0; i < ptrs && status == 0; i++) {
        uint32_t child = read_le32(pointers + (size_t)i * 4U);
        if (!child) continue;
        if (child >= mnt->blocks_count) status = -EIO;
        else if (depth == 0) status = clear_ext4_blocks(mnt, context, child, 1, freed);
        else status = free_ext4_indirect(mnt, child, depth - 1, context, freed);
    }
    if (status == 0) status = clear_ext4_blocks(mnt, context, block, 1, freed);
    free(pointers);
    return status;
}

static int free_ext4_legacy_tree(ext4_mount_t *mnt, uint8_t *raw, ext4_write_context_t *context, uint32_t *freed) {
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t block = read_le32(raw + 40 + i * 4U);
        if (block) {
            int status = clear_ext4_blocks(mnt, context, block, 1, freed);
            if (status < 0) return status;
            write_le32(raw + 40 + i * 4U, 0);
        }
    }
    for (uint32_t level = 0; level < 3; level++) {
        uint32_t indirect = read_le32(raw + 88 + level * 4U);
        if (!indirect) continue;
        int status = free_ext4_indirect(mnt, indirect, level, context, freed);
        if (status < 0) return status;
        write_le32(raw + 88 + level * 4U, 0);
    }
    return 0;
}

static int remove_ext4_dirent(ext4_mount_t *mnt, const uint8_t *parent_raw, const char *name, size_t name_length, uint64_t *dir_block, uint8_t *dir_data) {
    uint64_t parent_size = read_le32(parent_raw + 4);
    if (!name || !name_length || !dir_block || !dir_data) return -EINVAL;
    for (uint64_t block_offset = 0; block_offset < parent_size; block_offset += mnt->block_size) {
        uint64_t logical64 = block_offset / mnt->block_size;
        if (logical64 > UINT32_MAX) return -EFBIG;
        uint64_t physical;
        int mapped = map_ext4_parent_block(mnt, parent_raw, (uint32_t)logical64, &physical);
        if (mapped < 0) return mapped;
        if (!mapped) continue;
        int status = read_ext4_block(mnt, physical, dir_data);
        if (status < 0) return status;
        uint32_t pos = 0;
        uint32_t previous = 0;
        bool has_previous = false;
        while (pos < mnt->block_size) {
            if (pos + 8 > mnt->block_size) return -EIO;
            uint32_t ino = read_le32(dir_data + pos);
            uint16_t rec_len = read_le16(dir_data + pos + 4);
            uint8_t current_length = dir_data[pos + 6];
            if (rec_len == 0 && ino == 0) rec_len = mnt->block_size - pos;
            if (rec_len < 8 || (rec_len & 3) || rec_len > mnt->block_size - pos) return -EIO;
            if (ino != 0 && current_length == name_length && memcmp(dir_data + pos + 8, name, name_length) == 0) {
                if (!has_previous) write_le32(dir_data + pos, 0);
                else write_le16(dir_data + previous + 4, read_le16(dir_data + previous + 4) + rec_len);
                *dir_block = physical;
                return 0;
            }
            previous = pos;
            has_previous = true;
            pos += rec_len;
        }
    }
    return -ENOENT;
}

static int free_ext4_inode_bit(ext4_mount_t *mnt, uint8_t super[EXT4_SUPER_SIZE], uint32_t ino, uint8_t desc[64], uint8_t **bitmap, uint64_t *bitmap_block, uint32_t *group) {
    if (mnt->inodes_per_group & 7U) return -EIO;
    uint32_t current = (ino - 1) / mnt->inodes_per_group;
    uint32_t bit = (ino - 1) % mnt->inodes_per_group;
    int status = read_ext4_group_desc(mnt, current, desc);
    if (status < 0) return status;
    status = validate_group_checksum(mnt, current, desc);
    if (status < 0) return status;
    if (read_le16(desc + 0x12) & EXT4_BG_INODE_UNINIT) return -EIO;
    uint64_t map_block = read_le32(desc + 4);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) map_block |= (uint64_t)read_le32(desc + 0x24) << 32;
    if (!map_block || map_block >= mnt->blocks_count) return -EIO;
    uint32_t bitmap_bytes = mnt->inodes_per_group >> 3;
    uint8_t *map = malloc(mnt->block_size);
    if (!map) return -ENOMEM;
    status = read_ext4_block(mnt, map_block, map);
    if (status < 0) { free(map); return status; }
    if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), map, bitmap_bytes);
        uint32_t stored = read_le16(desc + 0x1A);
        if (mnt->desc_size >= 64) stored |= (uint32_t)read_le16(desc + 0x3A) << 16;
        else checksum &= 0xFFFF;
        if (stored != checksum) { free(map); return -EIO; }
    }
    if (!(map[bit >> 3] & (1U << (bit & 7U)))) { free(map); return -EIO; }
    map[bit >> 3] &= ~(1U << (bit & 7U));
    uint32_t free_count = read_le16(desc + 0x0E);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) free_count |= (uint32_t)read_le16(desc + 0x2E) << 16;
    if (free_count >= mnt->inodes_per_group) { free(map); return -EIO; }
    free_count++;
    write_le16(desc + 0x0E, (uint16_t)free_count);
    if ((mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && mnt->desc_size >= 64) write_le16(desc + 0x2E, (uint16_t)(free_count >> 16));
    if (mnt->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t checksum = calculate_crc32c(calculate_ext4_seed(mnt), map, bitmap_bytes);
        write_le16(desc + 0x1A, (uint16_t)checksum);
        if (mnt->desc_size >= 64) write_le16(desc + 0x3A, (uint16_t)(checksum >> 16));
    }
    update_group_checksum(mnt, current, desc);
    uint32_t super_free = read_le32(super + 0x10);
    if (super_free >= mnt->inodes_count) { free(map); return -EIO; }
    write_le32(super + 0x10, super_free + 1);
    *bitmap = map;
    *bitmap_block = map_block;
    *group = current;
    return 0;
}

int truncate_ext4(const char *path, uint64_t length) {
    if (!path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, true, &ino, &inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((inode.mode & S_IFMT) == S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -EISDIR; }
    if ((inode.mode & S_IFMT) != S_IFREG) { spin_unlock_irqrestore(&ext4_lock, irq); return -EINVAL; }
    if (inode.flags & EXT4_IMMUTABLE_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    if (inode.flags & EXT4_APPEND_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    if (length / mnt->block_size > EXT4_MAX_LOGICAL_BLOCK) { spin_unlock_irqrestore(&ext4_lock, irq); return -EFBIG; }
    ext4_inode_location_t location;
    status = locate_ext4_inode(mnt, ino, &location);
    uint8_t *raw = NULL;
    ext4_write_context_t context = {0};
    uint8_t super[EXT4_SUPER_SIZE];
    uint32_t freed = 0;
    bool committed = false;
    if (status == 0) {
        raw = malloc(mnt->inode_size);
        if (!raw) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, raw, mnt->inode_size, location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, ino, raw);
    if (status == 0 && length < inode.size) {
        uint32_t cutoff = (uint32_t)((length + mnt->block_size - 1) / mnt->block_size);
        if (inode.flags & EXT4_EXTENTS_FL) {
            if (read_le16(raw + 46) != 0) status = -EOPNOTSUPP;
            else status = truncate_ext4_extents(mnt, raw, cutoff, &context, &freed);
        } else status = truncate_ext4_legacy(mnt, raw, cutoff, &context, &freed);
    }
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    if (status == 0) {
        write_le32(raw + 4, (uint32_t)length);
        write_le32(raw + 108, (uint32_t)(length >> 32));
        uint64_t sectors = (uint64_t)freed * (mnt->block_size / 512U);
        uint64_t blocks_512 = read_le32(raw + 28);
        if (sectors > blocks_512) status = -EIO;
        else write_le32(raw + 28, (uint32_t)(blocks_512 - sectors));
    }
    if (status == 0) {
        write_le32(raw + 12, now_sec);
        write_le32(raw + 16, now_sec);
        uint16_t extra = mnt->inode_size >= 132 ? read_le16(raw + 128) : 0;
        if (extra >= 12) {
            write_le32(raw + 132, now_extra);
            write_le32(raw + 136, now_extra);
        }
        update_inode_checksum(mnt, ino, raw);
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && mnt->has_journal) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            finalize_group_states(mnt, &context);
            ext4_journal_batch_t batch = {0};
            status = stage_superblock(mnt, &batch, super);
            for (size_t i = 0; i < context.group_count && status == 0; i++) {
                status = stage_full_block(mnt, &batch, context.groups[i].bitmap_block, context.groups[i].bitmap);
                if (status == 0) status = stage_group_descriptor(mnt, &batch, context.groups[i].group, context.groups[i].descriptor);
            }
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &location, raw, mnt->inode_size);
            if (status == 0) status = commit_staged_blocks(mnt, &batch);
            free_staged_batch(&batch);
        }
        if (status == 0) {
            mnt->free_blocks += freed;
            mnt->state = read_le16(super + 0x3A);
        }
        free_ext4_write_context(&context);
        free(raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    if (status == 0) status = commit_ext4_group_states(mnt, &context);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &location, raw);
    if (status == 0) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        }
    }
    if (status == 0) {
        mnt->free_blocks += freed;
        mnt->state = read_le16(super + 0x3A);
    } else if (committed) mnt->read_only = true;
    free_ext4_write_context(&context);
    free(raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

int unlink_ext4(const char *path) {
    if (!path) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOENT; }
    if (mnt->read_only) { spin_unlock_irqrestore(&ext4_lock, irq); return -EROFS; }
    (void)relative;
    char parent[256];
    char name[256];
    int status = split_ext4_parent(path, parent, name);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    const char *parent_relative;
    ext4_mount_t *parent_mnt = find_ext4_mount(parent, &parent_relative);
    if (parent_mnt != mnt) { spin_unlock_irqrestore(&ext4_lock, irq); return -EXDEV; }
    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    status = resolve_ext4_inode(mnt, parent_relative, true, &parent_ino, &parent_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((parent_inode.mode & S_IFMT) != S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -ENOTDIR; }
    if (parent_inode.flags & EXT4_IMMUTABLE_FL) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    uint32_t child_ino;
    ext4_inode_t child_inode;
    status = resolve_ext4_inode(mnt, relative, false, &child_ino, &child_inode);
    if (status < 0) { spin_unlock_irqrestore(&ext4_lock, irq); return status; }
    if ((child_inode.mode & S_IFMT) == S_IFDIR) { spin_unlock_irqrestore(&ext4_lock, irq); return -EISDIR; }
    if (child_inode.flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) { spin_unlock_irqrestore(&ext4_lock, irq); return -EPERM; }
    struct timespec now = time_get_realtime_ts();
    uint32_t now_sec = now.tv_sec < 0 ? 0 : (uint32_t)now.tv_sec;
    uint32_t now_nsec = (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) ? 0 : (uint32_t)now.tv_nsec;
    uint32_t now_extra = ((uint32_t)(((uint64_t)now.tv_sec >> 32) & 3U)) | (now_nsec << 2);
    ext4_inode_location_t parent_location;
    status = locate_ext4_inode(mnt, parent_ino, &parent_location);
    ext4_inode_location_t child_location;
    if (status == 0) status = locate_ext4_inode(mnt, child_ino, &child_location);
    uint8_t *parent_raw = NULL;
    uint8_t *child_raw = NULL;
    uint8_t *dir_data = NULL;
    uint64_t dir_block = 0;
    uint8_t *inode_map = NULL;
    uint64_t inode_map_block = 0;
    uint32_t inode_group = 0;
    uint8_t desc[64];
    uint8_t super[EXT4_SUPER_SIZE];
    ext4_write_context_t context = {0};
    uint32_t freed = 0;
    bool delete_inode = false;
    bool committed = false;
    if (status == 0) {
        parent_raw = malloc(mnt->inode_size);
        child_raw = malloc(mnt->inode_size);
        dir_data = malloc(mnt->block_size);
        if (!parent_raw || !child_raw || !dir_data) status = -ENOMEM;
    }
    if (status == 0) status = read_checked_device(mnt, parent_raw, mnt->inode_size, parent_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, parent_ino, parent_raw);
    if (status == 0 && (read_le32(parent_raw + 32) & EXT4_INDEX_FL)) status = -EOPNOTSUPP;
    if (status == 0) status = read_checked_device(mnt, child_raw, mnt->inode_size, child_location.byte_offset);
    if (status == 0) status = validate_inode_checksum(mnt, child_ino, child_raw);
    if (status == 0) status = remove_ext4_dirent(mnt, parent_raw, name, strlen(name), &dir_block, dir_data);
    if (status == 0) status = finalize_ext4_dir_block(mnt, parent_ino, parent_raw, dir_data);
    if (status == 0) {
        uint16_t links = read_le16(child_raw + 26);
        if (!links) status = -EIO;
        else {
            links--;
            write_le16(child_raw + 26, links);
            write_le32(child_raw + 12, now_sec);
            uint16_t child_extra = mnt->inode_size >= 132 ? read_le16(child_raw + 128) : 0;
            if (child_extra >= 12) write_le32(child_raw + 132, now_extra);
            if (links == 0) {
                delete_inode = true;
                write_le32(child_raw + 20, now_sec);
                write_le32(child_raw + 4, 0);
                write_le32(child_raw + 108, 0);
                write_le32(child_raw + 28, 0);
                if (child_inode.flags & EXT4_EXTENTS_FL) {
                    if (read_le16(child_raw + 46) != 0) status = -EOPNOTSUPP;
                    else status = truncate_ext4_extents(mnt, child_raw, 0, &context, &freed);
                } else status = free_ext4_legacy_tree(mnt, child_raw, &context, &freed);
            }
            update_inode_checksum(mnt, child_ino, child_raw);
        }
    }
    if (status == 0) {
        write_le32(parent_raw + 12, now_sec);
        write_le32(parent_raw + 16, now_sec);
        uint16_t parent_extra = mnt->inode_size >= 132 ? read_le16(parent_raw + 128) : 0;
        if (parent_extra >= 12) {
            write_le32(parent_raw + 132, now_extra);
            write_le32(parent_raw + 136, now_extra);
        }
        update_inode_checksum(mnt, parent_ino, parent_raw);
    }
    if (status == 0) status = read_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status == 0) status = validate_superblock_checksum(mnt, super);
    if (status == 0 && delete_inode) status = free_ext4_inode_bit(mnt, super, child_ino, desc, &inode_map, &inode_map_block, &inode_group);
    if (status == 0 && mnt->has_journal) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            bool merged = delete_inode && merge_ext4_inode_desc(mnt, &context, inode_group, desc);
            finalize_group_states(mnt, &context);
            ext4_journal_batch_t batch = {0};
            status = stage_superblock(mnt, &batch, super);
            for (size_t i = 0; i < context.group_count && status == 0; i++) {
                status = stage_full_block(mnt, &batch, context.groups[i].bitmap_block, context.groups[i].bitmap);
                if (status == 0) status = stage_group_descriptor(mnt, &batch, context.groups[i].group, context.groups[i].descriptor);
            }
            if (status == 0 && delete_inode) status = stage_full_block(mnt, &batch, inode_map_block, inode_map);
            if (status == 0 && delete_inode && !merged) status = stage_group_descriptor(mnt, &batch, inode_group, desc);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &child_location, child_raw, mnt->inode_size);
            if (status == 0) status = stage_full_block(mnt, &batch, dir_block, dir_data);
            if (status == 0) status = stage_inode_bytes(mnt, &batch, &parent_location, parent_raw, mnt->inode_size);
            if (status == 0) status = commit_staged_blocks(mnt, &batch);
            free_staged_batch(&batch);
        }
        if (status == 0) {
            mnt->free_blocks += freed;
            mnt->state = read_le16(super + 0x3A);
        }
        free_ext4_write_context(&context);
        free(inode_map);
        free(child_raw);
        free(dir_data);
        free(parent_raw);
        spin_unlock_irqrestore(&ext4_lock, irq);
        return status;
    }
    if (status == 0) {
        write_le16(super + 0x3A, read_le16(super + 0x3A) & (uint16_t)~EXT4_STATE_CLEAN);
        update_superblock_checksum(mnt, super);
        status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        if (status == 0) committed = true;
    }
    bool merged = delete_inode && merge_ext4_inode_desc(mnt, &context, inode_group, desc);
    if (status == 0) status = commit_ext4_group_states(mnt, &context);
    if (status == 0 && delete_inode) status = write_ext4_block(mnt, inode_map_block, inode_map);
    if (status == 0 && delete_inode && !merged) status = write_ext4_group_desc(mnt, inode_group, desc);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &child_location, child_raw);
    if (status == 0) status = write_ext4_block(mnt, dir_block, dir_data);
    if (status == 0) status = write_ext4_inode_bytes(mnt, &parent_location, parent_raw);
    if (status == 0) {
        uint64_t free_blocks = combine_u32s(read_le32(super + 0x0C), (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le32(super + 0x158) : 0);
        if (freed > mnt->blocks_count - free_blocks) status = -ENOSPC;
        else {
            free_blocks += freed;
            write_le32(super + 0x0C, (uint32_t)free_blocks);
            if (mnt->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) write_le32(super + 0x158, (uint32_t)(free_blocks >> 32));
            write_le16(super + 0x3A, read_le16(super + 0x3A) | EXT4_STATE_CLEAN);
            update_superblock_checksum(mnt, super);
            status = write_checked_device(mnt, super, sizeof(super), EXT4_SUPER_OFFSET);
        }
    }
    if (status == 0) {
        mnt->free_blocks += freed;
        mnt->state = read_le16(super + 0x3A);
    } else if (committed) mnt->read_only = true;
    free_ext4_write_context(&context);
    free(inode_map);
    free(child_raw);
    free(dir_data);
    free(parent_raw);
    spin_unlock_irqrestore(&ext4_lock, irq);
    return status;
}

bool check_ext4_writable(const char *path) {
    if (!path) return false;
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    ext4_mount_t *mnt = find_ext4_mount(path, NULL);
    bool writable = mnt && !mnt->read_only;
    spin_unlock_irqrestore(&ext4_lock, irq);
    return writable;
}

int mount_ext4(const char *source, const char *path, unsigned long flags, const char *data) {
    if (!source || !source[0]) return -EINVAL;
    if (flags & ~(MS_RDONLY | MS_SILENT)) return -EOPNOTSUPP;
    if (data && data[0] && strcmp(data, "ro") != 0 && strcmp(data, "rw") != 0) return -EOPNOTSUPP;
    if (data && strcmp(data, "rw") == 0 && (flags & MS_RDONLY)) return -EINVAL;
    const char *dev = get_device_name(source);
    if (!dev || !*dev || strlen(dev) > 64 || !path || path[0] != '/' || strlen(path) > 63) return -EINVAL;

    uint64_t device_size;
    int status = get_block_device_size(dev, &device_size);
    if (status < 0) return status;
    if (device_size < EXT4_SUPER_OFFSET + EXT4_SUPER_SIZE) return -EINVAL;

    ext4_mount_t probe;
    memset(&probe, 0, sizeof(probe));
    strlcpy(probe.device, dev, sizeof(probe.device));
    probe.device_size = device_size;
    probe.read_only = (flags & MS_RDONLY) || (data && strcmp(data, "ro") == 0);
    uint8_t super[EXT4_SUPER_SIZE];
    status = read_checked_device(&probe, super, sizeof(super), EXT4_SUPER_OFFSET);
    if (status < 0) return status;
    if (read_le16(super + 0x38) != EXT4_SUPER_MAGIC) return -EINVAL;

    uint32_t log_block = read_le32(super + 0x18);
    if (log_block > 6) return -EOPNOTSUPP;
    probe.block_size = EXT4_MIN_BLOCK_SIZE << log_block;
    if (probe.block_size < EXT4_MIN_BLOCK_SIZE || probe.block_size > EXT4_MAX_BLOCK_SIZE) return -EOPNOTSUPP;
    probe.inodes_count = read_le32(super + 0x00);
    probe.first_data_block = read_le32(super + 0x14);
    probe.blocks_per_group = read_le32(super + 0x20);
    probe.inodes_per_group = read_le32(super + 0x28);
    probe.state = read_le16(super + 0x3A);
    probe.reserved_uid = read_le16(super + 0x50);
    probe.reserved_gid = read_le16(super + 0x52);
    probe.feature_compat = read_le32(super + 0x5C);
    probe.feature_incompat = read_le32(super + 0x60);
    probe.feature_ro_compat = read_le32(super + 0x64);
    memcpy(probe.uuid, super + 0x68, sizeof(probe.uuid));
    probe.reserved_gdt_blocks = read_le16(super + 0xCE);
    probe.checksum_type = super[0x175];
    probe.checksum_seed = read_le32(super + 0x270);

    bool wants_write = !probe.read_only;
    if (probe.feature_compat & ~EXT4_SUPPORTED_COMPAT) return -EOPNOTSUPP;
    if (probe.feature_incompat & ~EXT4_SUPPORTED_INCOMPAT) return -EOPNOTSUPP;
    if (probe.feature_ro_compat & ~EXT4_SUPPORTED_RO_COMPAT) return -EOPNOTSUPP;
    if (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) return -EUCLEAN;
    if (probe.feature_ro_compat & (EXT4_FEATURE_RO_COMPAT_BIGALLOC | EXT4_FEATURE_RO_COMPAT_VERITY | EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)) return -EOPNOTSUPP;
    if (wants_write && (probe.feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2)) return -EOPNOTSUPP;
    if (wants_write && (probe.feature_ro_compat & (EXT4_FEATURE_RO_COMPAT_QUOTA | EXT4_FEATURE_RO_COMPAT_PROJECT))) return -EOPNOTSUPP;
    if (wants_write && (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_READONLY)) return -EROFS;
    if (wants_write && (probe.state & (EXT4_STATE_ERRORS | EXT4_STATE_ORPHANS))) return -EUCLEAN;
    if (wants_write && !(probe.state & EXT4_STATE_CLEAN)) return -EUCLEAN;
    if (wants_write && (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) && probe.checksum_type != EXT4_CSUM_TYPE_CRC32C) return -EOPNOTSUPP;
    if (wants_write && (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) && read_le32(super + 0x48) != EXT4_OS_LINUX) return -EOPNOTSUPP;
    if (!probe.inodes_count || !probe.blocks_per_group || !probe.inodes_per_group) return -EINVAL;
    if (probe.blocks_per_group & 7U || probe.blocks_per_group > probe.block_size * 8U) return -EOPNOTSUPP;

    uint32_t revision = read_le32(super + 0x4c);
    probe.inode_size = revision == 0 ? EXT4_GOOD_OLD_INODE_SIZE : read_le16(super + 0x58);
    if (probe.inode_size < EXT4_GOOD_OLD_INODE_SIZE || probe.inode_size > probe.block_size || (probe.inode_size & (probe.inode_size - 1))) return -EINVAL;
    probe.desc_size = (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? read_le16(super + 0xFE) : 32;
    bool needs_64bit_desc = probe.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT;
    if (probe.desc_size < (needs_64bit_desc ? 64 : 32) || probe.desc_size > 64 || (probe.desc_size & 7)) return -EINVAL;
    uint64_t inode_span = (uint64_t)probe.inodes_per_group * probe.inode_size;
    if (inode_span > UINT64_MAX - probe.block_size + 1) return -EOVERFLOW;
    probe.inode_blocks_per_group = (inode_span + probe.block_size - 1) / probe.block_size;
    if (!probe.inode_blocks_per_group || probe.inode_blocks_per_group > UINT32_MAX) return -EFBIG;

    uint32_t blocks_hi = needs_64bit_desc ? read_le32(super + 0x150) : 0;
    uint32_t reserved_hi = needs_64bit_desc ? read_le32(super + 0x154) : 0;
    uint32_t free_hi = needs_64bit_desc ? read_le32(super + 0x158) : 0;
    probe.blocks_count = combine_u32s(read_le32(super + 0x04), blocks_hi);
    probe.reserved_blocks = combine_u32s(read_le32(super + 0x08), reserved_hi);
    probe.free_blocks = combine_u32s(read_le32(super + 0x0C), free_hi);
    if (probe.blocks_count <= probe.first_data_block || probe.free_blocks > probe.blocks_count) return -EINVAL;
    if (wants_write) {
        status = validate_superblock_checksum(&probe, super);
        if (status < 0) return status;
    }
    uint64_t fs_bytes;
    if (probe.blocks_count > UINT64_MAX / probe.block_size) return -EOVERFLOW;
    fs_bytes = probe.blocks_count * (uint64_t)probe.block_size;
    if (fs_bytes > device_size) return -EINVAL;
    uint64_t data_blocks = probe.blocks_count - probe.first_data_block;
    uint64_t groups = (data_blocks + probe.blocks_per_group - 1) /
                      probe.blocks_per_group;
    if (!groups || groups > UINT32_MAX) return -EFBIG;
    probe.groups_count = (uint32_t)groups;
    probe.gdt_offset = probe.block_size == 1024 ? 2048 : probe.block_size;
    if (probe.gdt_offset > fs_bytes || (uint64_t)probe.groups_count * probe.desc_size > fs_bytes - probe.gdt_offset) return -EINVAL;
    if (wants_write && (probe.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)) {
        status = open_ext4_journal(&probe, super);
        if (status < 0) return status;
    }
    if (wants_write && (probe.feature_compat & EXT4_FEATURE_COMPAT_ORPHAN_FILE)) {
        status = check_ext4_orphans(&probe, super);
        if (status < 0) return status;
    }

    char normalized[64];
    strlcpy(normalized, path, sizeof(normalized));
    size_t target_len = strlen(normalized);
    while (target_len > 1 && normalized[target_len - 1] == '/')
        normalized[--target_len] = '\0';
    strlcpy(probe.target, normalized, sizeof(probe.target));

    ext4_inode_t root;
    status = read_ext4_inode(&probe, EXT4_ROOT_INO, &root);
    if (status < 0) return status;
    if ((root.mode & S_IFMT) != S_IFDIR) return -EINVAL;
    if (wants_write) {
        uint8_t *raw = malloc(probe.inode_size);
        if (!raw) return -ENOMEM;
        status = read_ext4_inode_bytes(&probe, EXT4_ROOT_INO, raw);
        free(raw);
        if (status < 0) return status;
    }

    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    int slot = -1;
    for (int i = 0; i < EXT4_MAX_MOUNTS; i++) {
        if (ext4_mounts[i].active && strcmp(ext4_mounts[i].target, normalized) == 0) {
            spin_unlock_irqrestore(&ext4_lock, irq);
            return -EBUSY;
        }
        if (!ext4_mounts[i].active && slot < 0) slot = i;
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&ext4_lock, irq);
        return -ENOSPC;
    }
    ext4_mounts[slot] = probe;
    ext4_mounts[slot].active = true;
    spin_unlock_irqrestore(&ext4_lock, irq);
    return 0;
}

int unmount_ext4(const char *path) {
    if (!path) return -EINVAL;
    char normalized[64];
    if (strlen(path) >= sizeof(normalized)) return -ENAMETOOLONG;
    strlcpy(normalized, path, sizeof(normalized));
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/')
        normalized[--length] = '\0';
    uint64_t irq;
    spin_lock_irqsave(&ext4_lock, &irq);
    for (int i = 0; i < EXT4_MAX_MOUNTS; i++) {
        if (ext4_mounts[i].active && strcmp(ext4_mounts[i].target, normalized) == 0) {
            memset(&ext4_mounts[i], 0, sizeof(ext4_mounts[i]));
            spin_unlock_irqrestore(&ext4_lock, irq);
            return 0;
        }
    }
    spin_unlock_irqrestore(&ext4_lock, irq);
    return -ENOENT;
}

bool check_ext4_path(const char *path) {
    return path && find_ext4_mount(path, NULL) != NULL;
}

int stat_ext4(const char *path, struct stat *st, bool follow) {
    if (!path || !st) return -EINVAL;
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, follow, &ino, &inode);
    if (status < 0) return status;
    memset(st, 0, sizeof(*st));
    st->st_ino = ino;
    st->st_mode = inode.mode;
    st->st_nlink = inode.links;
    st->st_uid = inode.uid;
    st->st_gid = inode.gid;
    st->st_size = inode.size;
    st->st_blksize = mnt->block_size;
    st->st_blocks = inode.blocks_512;
    st->st_atime = inode.atime;
    st->st_mtime = inode.mtime;
    st->st_ctime = inode.ctime;
    return 0;
}

int statx_ext4_metadata(const char *path, struct statx *stx, bool follow) {
    if (!path || !stx) return -EINVAL;
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, follow, &ino, &inode);
    if (status < 0) return status;
    if (inode.has_btime) {
        stx->stx_btime.tv_sec = inode.btime.tv_sec;
        stx->stx_btime.tv_nsec = inode.btime.tv_nsec;
        stx->stx_mask |= STATX_BTIME;
    }
    stx->stx_attributes_mask |= STATX_ATTR_COMPRESSED | STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND | STATX_ATTR_NODUMP | STATX_ATTR_ENCRYPTED | STATX_ATTR_VERITY;
    if (inode.flags & EXT4_COMPR_FL) stx->stx_attributes |= STATX_ATTR_COMPRESSED;
    if (inode.flags & EXT4_IMMUTABLE_FL) stx->stx_attributes |= STATX_ATTR_IMMUTABLE;
    if (inode.flags & EXT4_APPEND_FL) stx->stx_attributes |= STATX_ATTR_APPEND;
    if (inode.flags & EXT4_NODUMP_FL) stx->stx_attributes |= STATX_ATTR_NODUMP;
    if (inode.flags & EXT4_ENCRYPT_FL) stx->stx_attributes |= STATX_ATTR_ENCRYPTED;
    if (inode.flags & EXT4_VERITY_FL) stx->stx_attributes |= STATX_ATTR_VERITY;
    return 0;
}

int64_t read_ext4(const char *path, void *buffer, uint64_t count, uint64_t offset) {
    if (!path || (!buffer && count)) return -EINVAL;
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, true, &ino, &inode);
    (void)ino;
    if (status < 0) return status;
    if ((inode.mode & S_IFMT) == S_IFDIR) return -EISDIR;
    if ((inode.mode & S_IFMT) != S_IFREG) return -EINVAL;
    return read_ext4_inode_data(mnt, &inode, buffer, count, offset);
}

int read_ext4_link(const char *path, char *buffer, size_t size) {
    if (!path || !buffer || !size) return -EINVAL;
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t ino;
    ext4_inode_t inode;
    int status = resolve_ext4_inode(mnt, relative, false, &ino, &inode);
    (void)ino;
    if (status < 0) return status;
    return read_ext4_symlink_inode(mnt, &inode, buffer, size);
}

static int select_ext4_direntry(uint32_t ino, uint8_t type, const char *name, uint8_t length, void *opaque) {
    ext4_readdir_context_t *ctx = opaque;
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    if (ctx->seen++ != ctx->wanted) return 0;
    if ((size_t)length + 1 > ctx->name_size) return -ENAMETOOLONG;
    memcpy(ctx->name, name, length);
    ctx->name[length] = '\0';
    switch (type) {
        case EXT4_FT_REG_FILE:
            *ctx->type = DT_REG;
            break;
        case EXT4_FT_DIR:
            *ctx->type = DT_DIR;
            break;
        case EXT4_FT_CHRDEV:
            *ctx->type = DT_CHR;
            break;
        case EXT4_FT_BLKDEV:
            *ctx->type = DT_BLK;
            break;
        case EXT4_FT_FIFO:
            *ctx->type = DT_FIFO;
            break;
        case EXT4_FT_SOCK:
            *ctx->type = DT_SOCK;
            break;
        case EXT4_FT_SYMLINK:
            *ctx->type = DT_LNK;
            break;
        default:
            *ctx->type = DT_UNKNOWN;
            break;
    }
    *ctx->ino = ino;
    return 1;
}

int get_next_ext4_child(int *index, const char *path, char *name, size_t name_size, uint8_t *type, ino_t *ino) {
    if (!index || *index < 0 || !path || !name || !name_size || !type || !ino) return -EINVAL;
    const char *relative;
    ext4_mount_t *mnt = find_ext4_mount(path, &relative);
    if (!mnt) return -ENOENT;
    uint32_t dir_ino;
    ext4_inode_t dir;
    int status = resolve_ext4_inode(mnt, relative, true, &dir_ino, &dir);
    (void)dir_ino;
    if (status < 0) return status;
    ext4_readdir_context_t ctx = {*index, 0, name, name_size, type, ino};
    status = walk_ext4_directory(mnt, &dir, select_ext4_direntry, &ctx);
    if (status == 1) {
        (*index)++;
        return 0;
    }
    return status < 0 ? status : -ENOENT;
}
