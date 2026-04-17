#include <freestanding/stddef.h>
#include <main/errno.h>
#include <main/hostname.h>
#include <main/spinlock.h>

static char current_hostname[HOSTNAME_MAX_LEN] = DEFAULT_HOSTNAME;
static spinlock_t hostname_lock = SPINLOCK_INIT;

int get_hostname(char *name, size_t len) {
    if (!name || len == 0) return -EINVAL;

    uint64_t irq;
    spin_lock_irqsave(&hostname_lock, &irq);

    size_t i;
    // Copy until we hit the size limit or the null terminator
    for (i = 0; i < (len - 1) && current_hostname[i] != '\0'; i++) {
        name[i] = current_hostname[i];
    }
    
    // Always ensure the caller's buffer gets null-terminated!
    name[i] = '\0';

    spin_unlock_irqrestore(&hostname_lock, irq);
    return 0; // 0 means success
}

int set_hostname(const char *name, size_t len) {
    if (!name) return -EINVAL;
    
    // Don't overflow our internal buffer
    if (len >= HOSTNAME_MAX_LEN) {
        len = HOSTNAME_MAX_LEN - 1; 
    }

    uint64_t irq;
    spin_lock_irqsave(&hostname_lock, &irq);

    // Safely copy the string byte by byte
    size_t i;
    for (i = 0; i < len && name[i] != '\0'; i++) {
        current_hostname[i] = name[i];
    }
    
    // Always ensure it is null-terminated!
    current_hostname[i] = '\0';

    spin_unlock_irqrestore(&hostname_lock, irq);
    return 0; // 0 means success
}