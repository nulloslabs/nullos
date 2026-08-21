#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <main/log.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/panic.h>
#include <main/strings.h>
#include <main/rng.h>
#include <main/sched.h>
#include <io/terminal.h>
#include <io/devices.h>
#include <io/devtmpfs.h>
#include <io/tty.h>
#include <io/pty.h>
#include <io/keyboard.h>
#include <io/ide.h>
#include <io/pata.h>
#include <io/atapi.h>
#include <io/sata.h>
#include <io/mbr.h>
#include <io/gpt.h>
#include <syscalls/syscall_impls.h>

devtmpfs_device_t devtmpfs_devices[MAX_DEVTMPFS_DEVICES];
spinlock_t devtmpfs_lock = SPINLOCK_INIT;

static int register_device_info(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int), int index, bool block, uint64_t size, device_bus_t bus) {
    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, dev_name) == 0) {
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return -EEXIST;
        }
    }

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active) {
            strncpy(devtmpfs_devices[i].name, dev_name, 64);
            devtmpfs_devices[i].name[64] = '\0';
            devtmpfs_devices[i].read = read_fn;
            devtmpfs_devices[i].write = write_fn;
            devtmpfs_devices[i].index = index;
            devtmpfs_devices[i].block = block;
            devtmpfs_devices[i].size = size;
            devtmpfs_devices[i].bus = bus;
            devtmpfs_devices[i].active = true;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return 0;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOMEM;
}

int register_device(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int)) {
    return register_device_info(name, read_fn, write_fn, 0, false, 0, DEV_BUS_NONE);
}

int register_device_idx(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int), int index) {
    return register_device_info(name, read_fn, write_fn, index, false, 0, DEV_BUS_NONE);
}

int register_block_device_idx(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int), int index, uint64_t size) {
    return register_device_info(name, read_fn, write_fn, index, true, size, DEV_BUS_NONE);
}

int register_disk_device_idx(const char *name, uint64_t (*read_fn)(void *, uint64_t, uint64_t, int), uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int), int index, uint64_t size, device_bus_t bus) {
    return register_device_info(name, read_fn, write_fn, index, true, size, bus);
}

int get_block_device_size(const char *name, uint64_t *size) {
    if (!name || !size) return -EINVAL;
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active || strcmp(devtmpfs_devices[i].name, dev_name) != 0) continue;
        if (!devtmpfs_devices[i].block) {
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return -ESPIPE;
        }
        *size = devtmpfs_devices[i].size;
        spin_unlock_irqrestore(&devtmpfs_lock, irq);
        return 0;
    }
    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOENT;
}

int get_block_device_bus(const char *name, device_bus_t *bus, int *disk_index) {
    if (!name || !bus || !disk_index) return -EINVAL;
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active || strcmp(devtmpfs_devices[i].name, dev_name) != 0) continue;
        if (!devtmpfs_devices[i].block) {
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return -ESPIPE;
        }
        *bus = devtmpfs_devices[i].bus;
        *disk_index = devtmpfs_devices[i].index;
        spin_unlock_irqrestore(&devtmpfs_lock, irq);
        return 0;
    }
    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOENT;
}

int get_device_mode(const char *name, mode_t *mode) {
    if (!name || !mode) return -EINVAL;
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active || strcmp(devtmpfs_devices[i].name, dev_name) != 0) continue;
        if (devtmpfs_devices[i].block) {
            *mode = S_IFBLK | 0600;
        } else if (strncmp(dev_name, "fb", 2) == 0 || strcmp(dev_name, "console") == 0 || (strncmp(dev_name, "tty", 3) == 0 && dev_name[3] >= '0' && dev_name[3] <= '9' && dev_name[4] == '\0')) {
            *mode = S_IFCHR | 0600;
        } else {
            *mode = S_IFCHR | 0666;
        }
        spin_unlock_irqrestore(&devtmpfs_lock, irq);
        return 0;
    }
    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOENT;
}

int unregister_device(const char* name) {
    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, dev_name) == 0) {
            devtmpfs_devices[i].active = false;
            devtmpfs_devices[i].name[0] = '\0';
            devtmpfs_devices[i].read = NULL;
            devtmpfs_devices[i].write = NULL;
            devtmpfs_devices[i].block = false;
            devtmpfs_devices[i].size = 0;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return 0;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOENT;
}

static uint64_t null_read(void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)buf; (void)count; (void)offset; (void)dev_idx;
    return 0; 
}

static uint64_t null_write(const void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)buf; (void)offset; (void)dev_idx;
    return count; 
}

static uint64_t zero_read(void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset; (void)dev_idx;
    memset(buf, 0, count);
    return count;
}

static uint64_t zero_write(const void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)buf; (void)offset; (void)dev_idx;
    return count;
}

#define DEFINE_FB_CALLBACKS(n) static uint64_t fb##n##_read(void* buf, uint64_t count, uint64_t offset, int dev_idx) { \
    (void)dev_idx; return fb_read_index(n, buf, count, offset); \
} \
static uint64_t fb##n##_write(const void* buf, uint64_t count, uint64_t offset, int dev_idx) { \
    (void)dev_idx; return fb_write_index(n, buf, count, offset); \
}

DEFINE_FB_CALLBACKS(0)
DEFINE_FB_CALLBACKS(1)
DEFINE_FB_CALLBACKS(2)
DEFINE_FB_CALLBACKS(3)
DEFINE_FB_CALLBACKS(4)
DEFINE_FB_CALLBACKS(5)
DEFINE_FB_CALLBACKS(6)
DEFINE_FB_CALLBACKS(7)

static uint64_t read_ptmx(void *buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)buf; (void)count; (void)offset; (void)dev_idx;
    return (uint64_t)-EIO;
}

static uint64_t write_ptmx(const void *buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)buf; (void)count; (void)offset; (void)dev_idx;
    return (uint64_t)-EIO;
}

static int get_tty_device_index(int dev_idx) {
    task_t *task;
    if (dev_idx == TTY_ACTIVE_INDEX) return keyboard_tty;
    if (dev_idx != TTY_CTTY_INDEX) return dev_idx;
    task = get_current_task_ptr();
    if (task && task->ctty_idx >= 0 && task->ctty_idx < NUM_TTYS) return task->ctty_idx;
    return keyboard_tty;
}

static uint64_t read_tty(void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset;
    spinlock_t *lk = &tty_lock;
    uint64_t irq;
    dev_idx = get_tty_device_index(dev_idx);
    spin_lock_irqsave(lk, &irq);
    tty_t *t = get_tty(dev_idx);
    int got = 0;
    if (t) got = read_tty_ring(&t->input, (char *)buf, (int)count);
    spin_unlock_irqrestore(lk, irq);
    return (uint64_t)got;
}

static uint64_t write_tty(const void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset;
    dev_idx = get_tty_device_index(dev_idx);
    tty_t *t = get_tty(dev_idx);
    bool do_onlcr = false;
    if (t && (t->termios.c_oflag & OPOST) && (t->termios.c_oflag & ONLCR)) {
        do_onlcr = true;
    }
    return write_terminal_tty(dev_idx, buf, count, do_onlcr);
}

static uint64_t read_urandom(void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset; (void)dev_idx;

    if (count == 0 || buf == NULL) return 0;

    uint8_t kernel_buffer[256];
    uint64_t bytes_read = 0;

    while (bytes_read < count) {
        uint64_t remaining = count - bytes_read;
        uint64_t chunk_size = (remaining < sizeof(kernel_buffer)) ? remaining : sizeof(kernel_buffer);

        get_random_bytes(kernel_buffer, chunk_size);
        memcpy((uint8_t*)buf + bytes_read, kernel_buffer, chunk_size);

        bytes_read += chunk_size;
    }

    return bytes_read;
}
static uint64_t write_urandom(const void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset; (void)dev_idx;
    if (count == 0 || buf == NULL) return 0;
    add_entropy_bytes(buf, count);
    return count;
}

uint64_t read_device(const char *name, void *buf, uint64_t count, uint64_t offset) {
    while (*name == '/') name++;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active) continue;
        if (strcmp(devtmpfs_devices[i].name, name) != 0) continue;

        uint64_t (*read_fn)(void *, uint64_t, uint64_t, int) = devtmpfs_devices[i].read;
        int index = devtmpfs_devices[i].index;

        spin_unlock_irqrestore(&devtmpfs_lock, irq);

        if (!read_fn) return (uint64_t)-EPERM;
        return read_fn(buf, count, offset, index);
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return (uint64_t)-ENOENT;
}

uint64_t write_device(const char *name, const void *buf, uint64_t count, uint64_t offset) {
    while (*name == '/') name++;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (!devtmpfs_devices[i].active) continue;
        if (strcmp(devtmpfs_devices[i].name, name) != 0) continue;

        uint64_t (*write_fn)(const void *, uint64_t, uint64_t, int) = devtmpfs_devices[i].write;
        int index = devtmpfs_devices[i].index;

        spin_unlock_irqrestore(&devtmpfs_lock, irq);

        if (!write_fn) return (uint64_t)-EPERM;
        return write_fn(buf, count, offset, index);
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return (uint64_t)-ENOENT;
}

void init_devices(void) {
    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        devtmpfs_devices[i].active = false;
        devtmpfs_devices[i].name[0] = '\0';
        devtmpfs_devices[i].read = NULL;
        devtmpfs_devices[i].write = NULL;
        devtmpfs_devices[i].block = false;
        devtmpfs_devices[i].size = 0;
        devtmpfs_devices[i].bus = DEV_BUS_NONE;
    }

    register_device("null", null_read, null_write);
    register_device("zero", zero_read, zero_write);

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        for (uint64_t i = 0; i < fb_req.response->framebuffer_count; i++) {
            if (i == 0) register_device("fb0", fb0_read, fb0_write);
            else if (i == 1) register_device("fb1", fb1_read, fb1_write);
            else if (i == 2) register_device("fb2", fb2_read, fb2_write);
            else if (i == 3) register_device("fb3", fb3_read, fb3_write);
            else if (i == 4) register_device("fb4", fb4_read, fb4_write);
            else if (i == 5) register_device("fb5", fb5_read, fb5_write);
            else if (i == 6) register_device("fb6", fb6_read, fb6_write);
            else if (i == 7) register_device("fb7", fb7_read, fb7_write);
            else panic("too many framebuffers");
        }
    }

    register_device_idx("tty",     read_tty, write_tty, TTY_CTTY_INDEX);
    register_device_idx("console", read_tty, write_tty, TTY_ACTIVE_INDEX);
    register_device_idx("tty0",    read_tty, write_tty, TTY_ACTIVE_INDEX);
    register_device_idx("tty1",    read_tty, write_tty, 1);
    register_device_idx("tty2",    read_tty, write_tty, 2);
    register_device_idx("tty3",    read_tty, write_tty, 3);
    register_device_idx("tty4",    read_tty, write_tty, 4);
    register_device_idx("tty5",    read_tty, write_tty, 5);
    register_device_idx("tty6",    read_tty, write_tty, 6);
    register_device_idx("tty7",    read_tty, write_tty, 7);

    register_device("ptmx", read_ptmx, write_ptmx);

    register_device("urandom", read_urandom, write_urandom);

    {
        char name[24];
        uint64_t size;

        if (is_pata_present) {
            for (int i = 0; i < IDE_MAX_DEVICES; i++) {
                if (!pata_device_size(i, &size)) continue;
                if (!make_ide_disk_name(name, sizeof(name), "hd", i)) continue;
                if (register_disk_device_idx(name, read_pata_device, write_pata_device, i, size, DEV_BUS_PATA) < 0) {
                    log("devices: unable to register %s\n", name);
                    continue;
                }
                // Probe for GPT/MBR partitions on PATA disks
                if (!probe_gpt_for_pata_disk(i, name, size)) probe_mbr_for_pata_disk(i, name, size);
            }
        }

        if (is_atapi_present) {
            for (int i = 0; i < IDE_MAX_DEVICES; i++) {
                if (!atapi_device_size(i, &size)) continue;
                if (!make_ide_numbered_name(name, sizeof(name), "sr", i)) continue;
                if (register_block_device_idx(name, read_atapi_device, write_atapi_device, i, size) < 0) {
                    log("devices: unable to register %s\n", name);
                }
            }
        }

        if (is_sata_present) {
            for (int i = 0; i < sata_device_count(); i++) {
                if (!sata_device_size(i, &size)) continue;
                if (!make_sata_disk_name(name, sizeof(name), i)) continue;
                if (register_disk_device_idx(name, read_sata_device, write_sata_device, i, size, DEV_BUS_SATA) < 0) {
                    log("devices: unable to register %s\n", name);
                    continue;
                }
                if (!probe_gpt_for_sata_disk(i, name, size)) probe_mbr_for_sata_disk(i, name, size);
            }
        }
    }

    log("devices: initialized devices\n");
}
