#pragma once

#define KERNEL_SYSNAME "Nullkrnl"

#ifndef KERNEL_RELEASE
#define KERNEL_RELEASE "unknown"
#endif

__attribute__((noreturn)) void kmain(void);
