#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

#define PAGE_SIZE 4096

// Per-process Virtual Memory Area tracking.
//
// The kernel does not need a full VMA tree; the goal is to back /proc/<pid>/maps
// with accurate address ranges, permission bits, and a (best-effort) backing
// object name.  We keep a fixed-size table of VMAs inside task_t and let the
// ELF loader, mmap, brk, and stack setup record regions into it.

#define VMA_MAX     64   // max tracked regions per process
#define VMA_NAME_MAX 64  // backing-object name length (NUL-terminated)

// Memory protection bits, matching the PROT_* values userspace passes to mmap.
#define VMA_PROT_READ  1
#define VMA_PROT_WRITE 2
#define VMA_PROT_EXEC  4

// Flags describing how a VMA was created.
#define VMA_FLAG_ANON   0x0001  // anonymous mapping (no backing file)
#define VMA_FLAG_STACK  0x0002  // process main stack
#define VMA_FLAG_HEAP   0x0004  // process heap (brk area)
#define VMA_FLAG_SHARED 0x0008  // MAP_SHARED (vs MAP_PRIVATE)

typedef struct {
    bool     used;
    uint64_t start;        // inclusive, page-aligned
    uint64_t end;          // exclusive, page-aligned
    int      prot;         // VMA_PROT_* bitmask
    int      flags;        // VMA_FLAG_* bitmask
    uint64_t offset;       // file offset for file-backed mappings
    char     name[VMA_NAME_MAX];  // backing object (file path or "[heap]")
} vma_t;

typedef struct {
    vma_t entries[VMA_MAX];
} vma_table_t;

// Initialize an empty VMA table (also used by the scheduler when reusing a
// task slot).
void vma_init(vma_table_t *tbl);

// Add a region, merging with adjacent VMAs that share prot/flags/name/offset.
// The range [start, end) must be page-aligned and non-empty.
void vma_add(vma_table_t *tbl, uint64_t start, uint64_t end, int prot,
             int flags, uint64_t offset, const char *name);

// Remove all VMAs overlapping any part of [start, end), shrinking/splitting
// neighbors as needed.  Used by munmap.
void vma_remove(vma_table_t *tbl, uint64_t start, uint64_t end);

// Apply new protection bits to all VMAs overlapping [start, end).  Used by
// mprotect.
void vma_protect(vma_table_t *tbl, uint64_t start, uint64_t end, int prot);

// Grow/shrink the single "[heap]" VMA so /proc/<pid>/maps reflects the current
// brk.  `brk_start` is the heap origin; `brk` is the current break.
void vma_set_heap(vma_table_t *tbl, uint64_t brk_start, uint64_t brk);

// Iterator: copy the n-th VMA (in ascending address order) into `out`.
// Returns true on success, false when n is out of range.  Used by procfs to
// render /proc/<pid>/maps.
bool get_vma(const vma_table_t *tbl, int n, vma_t *out);

// Number of VMAs currently tracked.
int vma_count(const vma_table_t *tbl);
