#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

// Built-in to the libc for detecting stack smashing. (also required by GCC if not using -fno-stack-protector)

uintptr_t __stack_chk_guard = 0x0000736F6C6C756E; // Little easter-egg, convert it so you can find out what it is :)

__attribute__((noreturn)) void __stack_chk_fail(void) {
    // Oh no! We have a smashed stack! Let's give an error and abort.
    const char msg[] = "Stack smashing detected!\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    abort();
    __builtin_unreachable();
}
