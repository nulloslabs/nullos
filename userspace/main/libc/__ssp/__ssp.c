#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <asm/prctl.h>

#if UINTPTR_MAX == UINT64_MAX
#define SSP_FALLBACK_GUARD ((uintptr_t)0x2d1f3b5a7c9e4100ULL)
#define SSP_MIX_CONST      ((uintptr_t)0x9e3779b97f4a7c15ULL)
#else
#define SSP_FALLBACK_GUARD ((uintptr_t)0x2d1f3b00UL)
#define SSP_MIX_CONST      ((uintptr_t)0x9e3779b9UL)
#endif

#define AT_NULL   0
#define AT_RANDOM 25

typedef struct {
    void *self;
    void *dtv;
    void *self2;
    uintptr_t unused[2];
    uintptr_t stack_guard;
    uintptr_t pointer_guard;
} initial_tcb_t;

static initial_tcb_t initial_tcb;
uintptr_t __stack_chk_guard = SSP_FALLBACK_GUARD;

__attribute__((no_stack_protector)) static uintptr_t ssp_mix(uintptr_t x) {
    x ^= x >> 33;
    x *= SSP_MIX_CONST;
    x ^= x >> 29;
    return x;
}

__attribute__((no_stack_protector)) void __libc_init_ssp(uintptr_t *stack) {
    uintptr_t guard = SSP_FALLBACK_GUARD ^ (uintptr_t)&guard ^ (uintptr_t)stack;

    if (stack) {
        uintptr_t argc = stack[0];
        uintptr_t *p = stack + 1 + argc + 1;
        guard ^= ssp_mix(argc);

        while (*p) {
            guard ^= ssp_mix(*p++);
        }
        p++;

        while (p[0] != AT_NULL) {
            if (p[0] == AT_RANDOM && p[1]) {
                uintptr_t *random = (uintptr_t *)p[1];
                guard ^= random[0];
#if UINTPTR_MAX == UINT64_MAX
                guard ^= random[1];
#endif
                break;
            }
            guard ^= ssp_mix(p[0] ^ p[1]);
            p += 2;
        }
    }

    guard &= ~(uintptr_t)0xff;
    if (!guard) guard = SSP_FALLBACK_GUARD;

    initial_tcb.self = &initial_tcb;
    initial_tcb.dtv = 0;
    initial_tcb.self2 = &initial_tcb;
    initial_tcb.stack_guard = guard;
    initial_tcb.pointer_guard = ssp_mix(guard ^ (uintptr_t)&initial_tcb);

    __stack_chk_guard = guard;
    arch_prctl(ARCH_SET_FS, (uintptr_t)&initial_tcb);
}

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    static const char msg[] = "libc: stack smashing detected\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(127);
    __builtin_unreachable();
}

__attribute__((noreturn, no_stack_protector, visibility("hidden"))) void __stack_chk_fail_local(void) { __stack_chk_fail(); }
