#pragma once

#include <mm/pmm.h>

#define KERNEL_STACK_SIZE (8 * PAGE_SIZE)
#define KERNEL_STACK_GUARD_SIZE PAGE_SIZE
#define KERNEL_STACK_AREA_BASE 0xffffa00000000000ULL
#define KERNEL_STACK_SLOT_COUNT 2048
#define KERNEL_STACK_PAGES (KERNEL_STACK_SIZE / PAGE_SIZE)
#define KERNEL_STACK_SLOT_SIZE (KERNEL_STACK_SIZE + 2 * KERNEL_STACK_GUARD_SIZE)

#ifndef __ASSEMBLY__
#include <stdbool.h>
#include <stdint.h>
#include <mm/vmm.h>

void *alloc_kernel_stack(void);
void free_kernel_stack(void *stack);
void *kernel_stack_top(void *stack);
bool is_kernel_stack_guard(uint64_t address);
#endif
