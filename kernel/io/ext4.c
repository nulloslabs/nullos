#include <stdbool.h>
#include <errno.h>
#include <main/string.h>
#include <io/devices.h>
#include <io/ext4.h>
#include <sys/statx.h>
#include <mm/mm.h>

// WARNING:
// "hey where is ext2 and ext3?"
// They are supported by this ext4 reader.

ext4_mount_t ext4_mounts[EXT4_MAX_MOUNTS];
spinlock_t ext4_lock = SPINLOCK_INIT;

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t combine_u32s(uint32_t lo, uint32_t hi) {
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static int read_checked_device(const ext4_mount_t *mnt, void *buf, uint64_t count, uint64_t offset) {
    if (!mnt || !buf) return -EINVAL;
    if (offset > mnt->device_size || count > mnt->device_size - offset) return -EIO;
    uint64_t got = read_device(mnt->device, buf, count, offset);
    if ((int64_t)got < 0) return (int)(int64_t)got;
    return got == count ? 0 : -EIO;
}

static int read_ext4_block(const ext4_mount_t *mnt, uint64_t block, void *buf) {
    if (block >= mnt->blocks_count) return -EIO;
    if (block > UINT64_MAX / mnt->block_size) return -EOVERFLOW;
    return read_checked_device(mnt, buf, mnt->block_size, block * (uint64_t)mnt->block_size);
}

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
    inode->links = read_le16(raw + 26);
    inode->blocks_512 = read_le32(raw + 28);
    inode->flags = read_le32(raw + 32);
    inode->generation = read_le32(raw + 100);
    if (raw_size >= 152 && read_le16(raw + 128) >= 24) {
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
            if (unwritten) {
                *physical = 0;
                return 0;
            }
            uint64_t start = (uint64_t)read_le32(ex + 8) |
                             ((uint64_t)read_le16(ex + 6) << 32);
            if (start >= mnt->blocks_count || logical - first >= mnt->blocks_count - start) return -EIO;
            *physical = start + (logical - first);
            return 1;
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
        if (!mapped) {
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

int mount_ext4(const char *source, const char *target) {
    const char *dev = get_device_name(source);
    if (!dev || !*dev || strlen(dev) > 64 || !target || target[0] != '/' || strlen(target) > 63) return -EINVAL;

    uint64_t device_size;
    int status = get_block_device_size(dev, &device_size);
    if (status < 0) return status;
    if (device_size < EXT4_SUPER_OFFSET + EXT4_SUPER_SIZE) return -EINVAL;

    ext4_mount_t probe;
    memset(&probe, 0, sizeof(probe));
    strlcpy(probe.device, dev, sizeof(probe.device));
    probe.device_size = device_size;
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
    probe.feature_compat = read_le32(super + 0x5c);
    probe.feature_incompat = read_le32(super + 0x60);
    probe.feature_ro_compat = read_le32(super + 0x64);

    if (probe.feature_incompat & ~EXT4_SUPPORTED_INCOMPAT) return -EOPNOTSUPP;
    if (probe.feature_ro_compat & ~EXT4_SUPPORTED_RO_COMPAT) return -EOPNOTSUPP;
    if (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) return -EUCLEAN;
    if (probe.feature_ro_compat & (EXT4_FEATURE_RO_COMPAT_BIGALLOC | EXT4_FEATURE_RO_COMPAT_VERITY | EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)) return -EOPNOTSUPP;
    if (!probe.inodes_count || !probe.blocks_per_group || !probe.inodes_per_group) return -EINVAL;

    uint32_t revision = read_le32(super + 0x4c);
    probe.inode_size = revision == 0 ? EXT4_GOOD_OLD_INODE_SIZE
                                     : read_le16(super + 0x58);
    if (probe.inode_size < EXT4_GOOD_OLD_INODE_SIZE || probe.inode_size > probe.block_size || (probe.inode_size & (probe.inode_size - 1))) return -EINVAL;
    probe.desc_size = (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                          ? read_le16(super + 0xfe)
                          : 32;
    if (probe.desc_size < 32 || probe.desc_size > 64 || (probe.desc_size & 7)) return -EINVAL;

    uint32_t blocks_hi = (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                             ? read_le32(super + 0x150)
                             : 0;
    probe.blocks_count = combine_u32s(read_le32(super + 0x04), blocks_hi);
    if (probe.blocks_count <= probe.first_data_block) return -EINVAL;
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

    char normalized[64];
    strlcpy(normalized, target, sizeof(normalized));
    size_t target_len = strlen(normalized);
    while (target_len > 1 && normalized[target_len - 1] == '/')
        normalized[--target_len] = '\0';
    strlcpy(probe.target, normalized, sizeof(probe.target));

    ext4_inode_t root;
    status = read_ext4_inode(&probe, EXT4_ROOT_INO, &root);
    if (status < 0) return status;
    if ((root.mode & S_IFMT) != S_IFDIR) return -EINVAL;

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

int unmount_ext4(const char *target) {
    if (!target) return -EINVAL;
    char normalized[64];
    if (strlen(target) >= sizeof(normalized)) return -ENAMETOOLONG;
    strlcpy(normalized, target, sizeof(normalized));
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
