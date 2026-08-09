#pragma once

#define KERNEL_SYSNAME "Nullkrnl"
#define KERNEL_MAJOR 1
#define KERNEL_MINOR 2
#define KERNEL_PATCH 0

__attribute__((noreturn)) void kmain(void);
