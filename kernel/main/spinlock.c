#include <main/spinlock.h>

void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            asm volatile("pause");
        }
    }
}

void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(lock);
}

void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
    asm volatile("pushfq; pop %0; cli" : "=r"(*flags) :: "memory");
    spin_lock(lock);
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory");
}
