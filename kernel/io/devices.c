#include <freestanding/errno.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/panic.h>
#include <main/strings.h>
#include <main/rng.h>
#include <main/log.h>
#include <io/terminal.h>
#include <io/devices.h>
#include <io/devtmpfs.h>
#include <io/ttys.h>
#include <io/ptys.h>
#include <io/keyboard.h>
#include <syscalls/syscall_impls.h>

devtmpfs_device_t devtmpfs_devices[MAX_DEVTMPFS_DEVICES];
spinlock_t devtmpfs_lock = SPINLOCK_INIT;

int register_device(const char* name, 
    uint64_t (*read_fn)(void*, uint64_t, uint64_t, int), 
    uint64_t (*write_fn)(const void*, uint64_t, uint64_t, int)) {
    return register_device_idx(name, read_fn, write_fn, 0);
}

int register_device_idx(const char* name, 
    uint64_t (*read_fn)(void*, uint64_t, uint64_t, int), 
    uint64_t (*write_fn)(const void*, uint64_t, uint64_t, int), int index) {
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
            devtmpfs_devices[i].index = index;
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

// Non-blocking read from the active TTY's input ring buffer.
// Returns whatever bytes are available (0 if empty — the caller, sys_read,
// handles blocking by yielding to the scheduler). All /dev/ttyN reads go to
// tty0 today since there is no VT switching.
static uint64_t read_tty(void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset;
    spinlock_t *lk = &tty_lock;
    uint64_t irq;
    spin_lock_irqsave(lk, &irq);
    tty_t *t = get_tty(dev_idx);
    int got = 0;
    if (t) got = read_tty_ring(&t->input, (char *)buf, (int)count);
    spin_unlock_irqrestore(lk, irq);
    return (uint64_t)got;
}

static uint64_t write_tty(const void* buf, uint64_t count, uint64_t offset, int dev_idx) {
    (void)offset; (void)dev_idx; // For now, all TTYs share the same display
    for (uint64_t i = 0; i < count; i++) {
        putchar(((const char*)buf)[i]);
    }
    return count;
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

uint64_t read_device(const char* name, void* buf, uint64_t count, uint64_t offset) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, dev_name) == 0) {
            uint64_t (*read_fn)(void*, uint64_t, uint64_t, int) = devtmpfs_devices[i].read;
            int index = devtmpfs_devices[i].index;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);

            if (read_fn) {
                return read_fn(buf, count, offset, index);
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
            uint64_t (*write_fn)(const void*, uint64_t, uint64_t, int) = devtmpfs_devices[i].write;
            int index = devtmpfs_devices[i].index;
            spin_unlock_irqrestore(&devtmpfs_lock, irq);

            if (write_fn) {
                return write_fn(buf, count, offset, index);
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

    register_device_idx("tty",  read_tty, write_tty, 0);
    register_device_idx("console", read_tty, write_tty, 0);
    register_device_idx("tty0", read_tty, write_tty, 0);
    register_device_idx("tty1", read_tty, write_tty, 1);
    register_device_idx("tty2", read_tty, write_tty, 2);
    register_device_idx("tty3", read_tty, write_tty, 3);
    register_device_idx("tty4", read_tty, write_tty, 4);
    register_device_idx("tty5", read_tty, write_tty, 5);
    register_device_idx("tty6", read_tty, write_tty, 6);
    register_device_idx("tty7", read_tty, write_tty, 7);

    register_device("ptmx", read_ptmx, write_ptmx);

    register_device("urandom", read_urandom, write_urandom);

    log("initialized devices");
}
