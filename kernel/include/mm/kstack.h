#pragma once

#include <mm/pmm.h>

#define KSTACK_SIZE (8 * PAGE_SIZE)
#define KSTACK_GUARD_SIZE PAGE_SIZE
#define KSTACK_AREA_BASE 0xffffa00000000000ULL
#define KSTACK_SLOT_COUNT 2048
#define KSTACK_PAGES (KSTACK_SIZE / PAGE_SIZE)
#define KSTACK_SLOT_SIZE (KSTACK_SIZE + 2 * KSTACK_GUARD_SIZE)

#ifndef __ASSEMBLY__
#include <stdint.h>
#include <stdbool.h>
#include <mm/vmm.h>

void *alloc_kstack(void);
void free_kstack(void *stack);
void *kstack_top(void *stack);
bool is_kstack_guard(uint64_t address);
#endif
