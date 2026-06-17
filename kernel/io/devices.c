#include <io/devices.h>
#include <io/devtmpfs.h>
#include <io/ttys.h>
#include <io/ptys.h>
#include <io/terminal.h>
#include <io/keyboard.h>
#include <main/string.h>
#include <freestanding/errno.h>
#include <main/limine_req.h>
#include <main/panic.h>
#include <main/strings.h>

devtmpfs_device_t devtmpfs_devices[MAX_DEVTMPFS_DEVICES];
spinlock_t devtmpfs_lock = SPINLOCK_INIT;

int register_device(const char* name, 
    uint64_t (*read_fn)(void*, uint64_t, uint64_t), 
    uint64_t (*write_fn)(const void*, uint64_t, uint64_t)) {
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
            strncpy(devtmpfs_devices[i].name, dev_name, 63);
            devtmpfs_devices[i].name[65] = '\0';
            devtmpfs_devices[i].read = read_fn;
            devtmpfs_devices[i].write = write_fn;
            devtmpfs_devices[i].active = true;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return 0;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOMEM;
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
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return 0;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return -ENOENT;
}

static uint64_t null_read(void* buf, uint64_t count, uint64_t offset) {
    (void)buf; (void)count; (void)offset;
    return 0; 
}

static uint64_t null_write(const void* buf, uint64_t count, uint64_t offset) {
    (void)buf; (void)offset;
    return count; 
}

static uint64_t zero_read(void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    for (uint64_t i = 0; i < count; i++) {
        ((uint8_t*)buf)[i] = 0;
    }
    return count;
}

static uint64_t zero_write(const void* buf, uint64_t count, uint64_t offset) {
    (void)buf; (void)offset;
    return count;
}

#define DEFINE_FB_CALLBACKS(n) static uint64_t fb##n##_read(void* buf, uint64_t count, uint64_t offset) { \
    return fb_read_index(n, buf, count, offset); \
} \
static uint64_t fb##n##_write(const void* buf, uint64_t count, uint64_t offset) { \
    return fb_write_index(n, buf, count, offset); \
}

DEFINE_FB_CALLBACKS(0)
DEFINE_FB_CALLBACKS(1)
DEFINE_FB_CALLBACKS(2)
DEFINE_FB_CALLBACKS(3)
DEFINE_FB_CALLBACKS(4)
DEFINE_FB_CALLBACKS(5)
DEFINE_FB_CALLBACKS(6)
DEFINE_FB_CALLBACKS(7)

static uint64_t read_ptmx(void *buf, uint64_t count, uint64_t offset) {
    (void)buf; (void)count; (void)offset;
    return (uint64_t)-EIO;
}

static uint64_t write_ptmx(const void *buf, uint64_t count, uint64_t offset) {
    (void)buf; (void)count; (void)offset;
    return (uint64_t)-EIO;
}

static uint64_t tty_read(void* buf, uint64_t count, uint64_t offset) {
    (void)buf; (void)count; (void)offset;
    return 0;
}

static uint64_t tty_write(const void* buf, uint64_t count, uint64_t offset) {
    for (uint64_t i = 0; i < count; i++) {
        putc(((const char*)buf)[i]);
    }
    return count;
}

uint64_t read_device(const char* name, void* buf, uint64_t count, uint64_t offset) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, dev_name) == 0) {
            uint64_t (*read_fn)(void*, uint64_t, uint64_t) = devtmpfs_devices[i].read;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);

            if (read_fn) {
                return read_fn(buf, count, offset);
            }
            return (uint64_t)-EPERM;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return (uint64_t)-ENOENT;
}

uint64_t write_device(const char* name, const void* buf, uint64_t count, uint64_t offset) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, dev_name) == 0) {
            uint64_t (*write_fn)(const void*, uint64_t, uint64_t) = devtmpfs_devices[i].write;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);

            if (write_fn) {
                return write_fn(buf, count, offset);
            }
            return (uint64_t)-EPERM;
        }
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
    }

    register_device("null", null_read, null_write);
    register_device("zero", zero_read, zero_write);

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        for (int i = 0; i < fb_req.response->framebuffer_count; i++) {
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

    register_device("tty",  tty_read, tty_write);
    register_device("tty0", tty_read, tty_write);
    register_device("tty1", tty_read, tty_write);
    register_device("tty2", tty_read, tty_write);
    register_device("tty3", tty_read, tty_write);
    register_device("tty4", tty_read, tty_write);
    register_device("tty5", tty_read, tty_write);
    register_device("tty6", tty_read, tty_write);
    register_device("tty7", tty_read, tty_write);

    register_device("ptmx",     read_ptmx, write_ptmx);
    register_device("pts/ptmx", read_ptmx, write_ptmx);

    printf("devices: initialized devices\n");
}
