#include <freestanding/stddef.h>
#include <freestanding/errno.h>
#include <main/hostname.h>
#include <main/spinlocks.h>

static char current_hostname[HOSTNAME_MAX_LEN] = DEFAULT_HOSTNAME;
static spinlock_t hostname_lock = SPINLOCK_INIT;

/* I fucking hate this get/set hostname system, like why return int?? Just return a const char *!
   Oh well, gotta stay POSIX... */

int get_hostname(char *name, size_t len) {
    if (!name || len == 0) return -EINVAL;

    uint64_t irq;
    spin_lock_irqsave(&hostname_lock, &irq);

    size_t i;
    for (i = 0; i < (len - 1) && current_hostname[i] != '\0'; i++) { name[i] = current_hostname[i]; }
    
    name[i] = '\0';

    spin_unlock_irqrestore(&hostname_lock, irq);
    return 0;
}

int set_hostname(const char *name, size_t len) {
    if (!name) return -EINVAL;
    
    if (len >= HOSTNAME_MAX_LEN) return -ENAMETOOLONG;

    uint64_t irq;
    spin_lock_irqsave(&hostname_lock, &irq);

    size_t i;
    for (i = 0; i < len && name[i] != '\0'; i++) { current_hostname[i] = name[i]; }
    
    current_hostname[i] = '\0';

    spin_unlock_irqrestore(&hostname_lock, irq);
    return 0;
}