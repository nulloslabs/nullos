#include <main/devfs.h>
#include <main/string.h>
#include <main/errno.h>

static devfs_device_t devfs_devices[MAX_DEVFS_DEVICES];

// ==========================================
// Standard Devices
// ==========================================
static uint64_t null_read(void* buf, uint64_t count, uint64_t offset) {
    (void)buf;
    (void)count;
    (void)offset;
    return 0; 
}

static uint64_t null_write(const void* buf, uint64_t count, uint64_t offset) {
    (void)buf;
    (void)offset;
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
    (void)buf;
    (void)offset;
    return count;
}

void init_devfs(void) {
    for (int i = 0; i < MAX_DEVFS_DEVICES; i++) {
        devfs_devices[i].active = false;
        devfs_devices[i].name[0] = '\0';
        devfs_devices[i].read = NULL;
        devfs_devices[i].write = NULL;
    }

    register_devfs_device("null", null_read, null_write);
    register_devfs_device("zero", zero_read, zero_write);
}

int register_devfs_device(const char* name, 
    uint64_t (*read_fn)(void*, uint64_t, uint64_t), 
    uint64_t (*write_fn)(const void*, uint64_t, uint64_t)) 
{
    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

    for (int i = 0; i < MAX_DEVFS_DEVICES; i++) {
        if (!devfs_devices[i].active) {
            strncpy(devfs_devices[i].name, name, 63);
            devfs_devices[i].name[63] = '\0';
            devfs_devices[i].read = read_fn;
            devfs_devices[i].write = write_fn;
            devfs_devices[i].active = true;
            return 0;
        }
    }
    return -ENOMEM;
}

bool devfs_device_exists(const char* name) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    for (int i = 0; i < MAX_DEVFS_DEVICES; i++) {
        if (devfs_devices[i].active && strcmp(devfs_devices[i].name, dev_name) == 0) {
            return true;
        }
    }
    return false;
}

uint64_t read_devfs(const char* name, void* buf, uint64_t count, uint64_t offset) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    for (int i = 0; i < MAX_DEVFS_DEVICES; i++) {
        if (devfs_devices[i].active && strcmp(devfs_devices[i].name, dev_name) == 0) {
            if (devfs_devices[i].read) {
                return devfs_devices[i].read(buf, count, offset);
            }
            return (uint64_t)-EPERM;
        }
    }
    return (uint64_t)-ENOENT;
}

uint64_t write_devfs(const char* name, const void* buf, uint64_t count, uint64_t offset) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    for (int i = 0; i < MAX_DEVFS_DEVICES; i++) {
        if (devfs_devices[i].active && strcmp(devfs_devices[i].name, dev_name) == 0) {
            if (devfs_devices[i].write) {
                return devfs_devices[i].write(buf, count, offset);
            }
            return (uint64_t)-EPERM;
        }
    }
    return (uint64_t)-ENOENT;
}

const char *devfs_get_device_name(int index) {
    int count = 0;
    for (int i = 0; i < MAX_DEVFS_DEVICES; i++) {
        if (!devfs_devices[i].active) continue;
        if (count == index) return devfs_devices[i].name;
        count++;
    }
    return NULL;
}
