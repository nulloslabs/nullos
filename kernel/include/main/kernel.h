#pragma once

#define KERNEL_SYSNAME "Nullkrnl"

// C is a bitch when it comes to macros. We gotta do this weird thing to turn these 3 numbers into one string.
#define STR(x) #x
#define XSTR(x) STR(x)

#define KERNEL_MAJOR 1
#define KERNEL_MINOR 0
#define KERNEL_PATCH 0
#define KERNEL_RELEASE XSTR(KERNEL_MAJOR) "." XSTR(KERNEL_MINOR) "." XSTR(KERNEL_PATCH)

__attribute__((noreturn)) void kmain(void);
