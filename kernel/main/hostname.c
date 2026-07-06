#include <freestanding/stddef.h>
#include <freestanding/errno.h>
#include <main/hostname.h>
#include <main/spinlocks.h>
#include <main/string.h>

static char current_hostname[HOSTNAME_MAX_LEN] = DEFAULT_HOSTNAME;
static spinlock_t hostname_lock = SPINLOCK_INIT;

int get_hostname(char *name, size_t len) {
    if (!name || len == 0) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&hostname_lock, &irq);
    strlcpy(name, current_hostname, len);
    spin_unlock_irqrestore(&hostname_lock, irq);
    return 0;
}

int set_hostname(const char *name, size_t len) {
    if (!name) return -EINVAL;
    if (len >= HOSTNAME_MAX_LEN) return -ENAMETOOLONG;

    uint64_t irq;
    spin_lock_irqsave(&hostname_lock, &irq);
    strlcpy(current_hostname, name, len + 1);
    spin_unlock_irqrestore(&hostname_lock, irq);
    return 0;
}
