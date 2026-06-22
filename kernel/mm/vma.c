#include <mm/vma.h>
#include <main/string.h>

void vma_init(vma_table_t *tbl) {
    if (!tbl) return;
    __builtin_memset(tbl, 0, sizeof(*tbl));
}

static void set_name(char *dst, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, VMA_NAME_MAX - 1);
    dst[VMA_NAME_MAX - 1] = '\0';
}

static bool same_attrs(const vma_t *a, int prot, int flags, uint64_t offset,
                       const char *name) {
    if (a->prot != prot) return false;
    if (a->flags != flags) return false;
    // Two regions may only merge into one if the file offset continues
    // seamlessly across the boundary.
    if (!(flags & VMA_FLAG_ANON)) {
        if (a->offset + (a->end - a->start) != offset) return false;
    }
    // Names must match (treat NULL and "" as equal).
    const char *an = a->name;
    const char *bn = name ? name : "";
    if ((an[0] == '\0') != (bn[0] == '\0')) {
        // one empty, one not
        if (an[0] != '\0' || bn[0] != '\0') return false;
    }
    if (strncmp(an, bn, VMA_NAME_MAX) != 0) return false;
    return true;
}

void vma_add(vma_table_t *tbl, uint64_t start, uint64_t end, int prot,
             int flags, uint64_t offset, const char *name) {
    if (!tbl || start >= end) return;

    // First, drop anything inside [start,end) so we never record overlapping
    // regions; then attempt to merge with the immediate neighbors.
    vma_remove(tbl, start, end);

    // Find a free slot.
    int slot = -1;
    for (int i = 0; i < VMA_MAX; i++) {
        if (!tbl->entries[i].used) { slot = i; break; }
    }
    if (slot < 0) return;  // table full; the mapping still works, just untracked

    vma_t *v = &tbl->entries[slot];
    v->used   = true;
    v->start  = start;
    v->end    = end;
    v->prot   = prot;
    v->flags  = flags;
    v->offset = offset;
    set_name(v->name, name);

    // Try to merge with a region ending exactly at `start`.
    for (int i = 0; i < VMA_MAX; i++) {
        if (i == slot) continue;
        vma_t *p = &tbl->entries[i];
        if (!p->used) continue;
        if (p->end == start && same_attrs(p, prot, flags, offset, name)) {
            p->end = end;
            // Now see if a region begins exactly at the new end.
            for (int j = 0; j < VMA_MAX; j++) {
                if (j == i) continue;
                vma_t *q = &tbl->entries[j];
                if (!q->used) continue;
                if (q->start == end && q->prot == prot && q->flags == flags &&
                    same_attrs(q, prot, flags, q->offset, q->name) &&
                    (!(flags & VMA_FLAG_ANON) ||
                     strncmp(q->name, p->name, VMA_NAME_MAX) == 0)) {
                    p->end = q->end;
                    q->used = false;
                }
            }
            v->used = false;
            return;
        }
    }
    // Try to merge with a region starting exactly at `end`.
    for (int i = 0; i < VMA_MAX; i++) {
        if (i == slot) continue;
        vma_t *q = &tbl->entries[i];
        if (!q->used) continue;
        if (q->start == end && same_attrs(q, prot, flags, offset, name)) {
            q->start = start;
            // file offset shifts backward by the size we prepended
            if (!(flags & VMA_FLAG_ANON)) q->offset = offset;
            v->used = false;
            return;
        }
    }
}

void vma_remove(vma_table_t *tbl, uint64_t start, uint64_t end) {
    if (!tbl || start >= end) return;
    for (int i = 0; i < VMA_MAX; i++) {
        vma_t *v = &tbl->entries[i];
        if (!v->used) continue;
        if (v->end <= start || v->start >= end) continue;  // no overlap

        // Split / shrink this region.
        uint64_t v_start = v->start;
        uint64_t v_end   = v->end;

        if (start <= v_start && end >= v_end) {
            // Fully covered: drop it.
            v->used = false;
        } else if (start <= v_start && end < v_end) {
            // Trim head.
            if (!(v->flags & VMA_FLAG_ANON))
                v->offset += (end - v_start);
            v->start = end;
        } else if (start > v_start && end >= v_end) {
            // Trim tail.
            v->end = start;
        } else {
            // Hole in the middle: keep [v_start,start) and add [end,v_end).
            v->end = start;
            // Find a free slot for the upper piece.
            for (int j = 0; j < VMA_MAX; j++) {
                if (!tbl->entries[j].used) {
                    vma_t *u = &tbl->entries[j];
                    u->used   = true;
                    u->start  = end;
                    u->end    = v_end;
                    u->prot   = v->prot;
                    u->flags  = v->flags;
                    if (!(v->flags & VMA_FLAG_ANON))
                        u->offset = v->offset + (end - v_start);
                    else
                        u->offset = v->offset;
                    set_name(u->name, v->name);
                    break;
                }
            }
        }
    }
}

void vma_protect(vma_table_t *tbl, uint64_t start, uint64_t end, int prot) {
    if (!tbl || start >= end) return;
    // Remove then re-add the covered portions with the new protection, so the
    // resulting regions split/merge correctly.  We snapshot first since
    // vma_remove mutates the table.
    vma_t snap[VMA_MAX];
    __builtin_memcpy(snap, tbl->entries, sizeof(snap));

    for (int i = 0; i < VMA_MAX; i++) {
        vma_t *v = &snap[i];
        if (!v->used) continue;
        if (v->end <= start || v->start >= end) continue;

        uint64_t a = (v->start > start) ? v->start : start;
        uint64_t b = (v->end < end) ? v->end : end;
        uint64_t off = v->offset;
        if (!(v->flags & VMA_FLAG_ANON)) off += (a - v->start);

        vma_add(tbl, a & ~(uint64_t)(PAGE_SIZE - 1),
                (b + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1),
                prot, v->flags, off, v->name);
    }
}

void vma_set_heap(vma_table_t *tbl, uint64_t brk_start, uint64_t brk) {
    if (!tbl) return;
    // Drop any existing [heap] entry, then re-add if the heap is non-empty.
    for (int i = 0; i < VMA_MAX; i++) {
        vma_t *v = &tbl->entries[i];
        if (!v->used) continue;
        if (v->flags & VMA_FLAG_HEAP) { v->used = false; }
    }
    if (brk > brk_start) {
        vma_add(tbl, brk_start, brk,
                VMA_PROT_READ | VMA_PROT_WRITE, VMA_FLAG_ANON | VMA_FLAG_HEAP,
                0, "[heap]");
    }
}

static int cmp_vma(const void *a, const void *b) {
    const vma_t *va = (const vma_t *)a;
    const vma_t *vb = (const vma_t *)b;
    if (!va->used && !vb->used) return 0;
    if (!va->used) return 1;
    if (!vb->used) return -1;
    if (va->start < vb->start) return -1;
    if (va->start > vb->start) return 1;
    return 0;
}

bool get_vma(const vma_table_t *tbl, int n, vma_t *out) {
    if (!tbl || !out || n < 0) return false;
    // Build a sorted view without mutating the source.
    vma_t copy[VMA_MAX];
    __builtin_memcpy(copy, tbl->entries, sizeof(copy));
    // Simple insertion sort by start address; unused slots sink to the end.
    for (int i = 1; i < VMA_MAX; i++) {
        for (int j = i; j > 0; j--) {
            if (cmp_vma(&copy[j - 1], &copy[j]) > 0) {
                vma_t tmp = copy[j - 1];
                copy[j - 1] = copy[j];
                copy[j] = tmp;
            } else {
                break;
            }
        }
    }
    int idx = 0;
    for (int i = 0; i < VMA_MAX; i++) {
        if (!copy[i].used) continue;
        if (idx == n) {
            *out = copy[i];
            return true;
        }
        idx++;
    }
    return false;
}

int vma_count(const vma_table_t *tbl) {
    if (!tbl) return 0;
    int n = 0;
    for (int i = 0; i < VMA_MAX; i++) {
        if (tbl->entries[i].used) n++;
    }
    return n;
}
