#include <stdint.h>
#include <main/log.h>
#include <main/panic.h>
#include <main/rng.h>
#include <main/stack_protector.h>

uintptr_t __stack_chk_guard = 0x6c6e726b6c6c756eULL;

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    panic("kernel stack corruption detected");
}

__attribute__((no_stack_protector)) void init_stack_protector(void) {
    uintptr_t guard = 0;
    // Get random bytes from PRNG
    get_random_bytes(&guard, sizeof(guard));
    // If we dont have bytes from get_random_bytes(), use "nullkrnl" as guard
    if (guard == 0) guard = 0x6e756c6c6f734747ULL;
    __stack_chk_guard = guard;
    log("stack protector: initialized stack protector\n");
}

