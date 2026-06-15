#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Not exposed in any header, just use extern.
extern void __libc_init_ssp(uintptr_t *stack);

__attribute__((noreturn, no_stack_protector)) void __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
    (void)rtld_fini;

    environ = argv + argc + 1;

    if (argv && argv[0]) {
        __progname = strrchr(argv[0], '/');
        if (__progname) {
            __progname++;
        } else {
            __progname = argv[0];
        }
    }

    __libc_init_ssp((uintptr_t *)stack_end);

    if (init) init();

    int status = main(argc, argv, environ);

    if (fini) fini();

    exit(status);
    __builtin_unreachable();
}
