#include <errno.h>
#include <main/string.h>
#include <io/devtmpfs.h>

bool device_exists_on_devtmpfs(const char* name) {
    while (*name == '/') name++;

    uint64_t irq;
    spin_lock_irqsave(&devtmpfs_lock, &irq);

    for (int i = 0; i < MAX_DEVTMPFS_DEVICES; i++) {
        if (devtmpfs_devices[i].active && strcmp(devtmpfs_devices[i].name, name) == 0) {
            spin_unlock_irqrestore(&devtmpfs_lock, irq);
            return true;
        }
    }

    spin_unlock_irqrestore(&devtmpfs_lock, irq);
    return false;
}

const char *get_devtmpfs_device_name(int index) {
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
