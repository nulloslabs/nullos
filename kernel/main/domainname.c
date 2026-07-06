#include <freestanding/stddef.h>
#include <freestanding/errno.h>
#include <main/domainname.h>
#include <main/spinlocks.h>
#include <main/string.h>

static char current_domainname[DOMAINNAME_MAX_LEN] = DEFAULT_DOMAINNAME;
static spinlock_t domainname_lock = SPINLOCK_INIT;

int get_domainname(char *name, size_t len) {
    if (!name || len == 0) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&domainname_lock, &irq);
    strlcpy(name, current_domainname, len);
    spin_unlock_irqrestore(&domainname_lock, irq);
    return 0;
}

int set_domainname(const char *name, size_t len) {
    if (!name) return -EINVAL;
    if (len >= DOMAINNAME_MAX_LEN) return -ENAMETOOLONG;

    uint64_t irq;
    spin_lock_irqsave(&domainname_lock, &irq);
    strlcpy(current_domainname, name, len + 1);
    spin_unlock_irqrestore(&domainname_lock, irq);
    return 0;
}
