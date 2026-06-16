#include <io/devtmpfs.h>
#include <main/string.h>
#include <freestanding/errno.h>

bool devtmpfs_device_exists(const char* name) {
    const char *dev_name = name;
    while (*dev_name == '.' || *dev_name == '/') dev_name++;
    if (strncmp(dev_name, "dev/", 4) == 0) dev_name += 4;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, dev_name) == 0) {
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return true;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return false;
}

const char *devtmpfs_get_device_name(int index) {
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

