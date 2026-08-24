#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <errno.h>
#include <asm/unistd.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/fb.h>
#include <sys/statx.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/epoll.h>
#include <sys/sysmacros.h>
#include <main/limine_req.h>
#include <main/elf.h>
#include <main/hostname.h>
#include <main/timekeeping.h>
#include <main/mp.h>
#include <main/fd.h>
#include <main/signal.h>
#include <main/string.h>
#include <io/fb.h>
#include <io/devices.h>
#include <io/devpts.h>
#include <io/initrd.h>
#include <io/keyboard.h>
#include <io/pty.h>
#include <io/power.h>
#include <io/net.h>
#include <io/serial.h>
#include <io/tmpfs.h>
#include <io/ext4.h>
#include <io/iso9660.h>
#include <io/vfat.h>
#include <io/gpt.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/mm.h>

void sys_mmap(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;
    int      flags  = (int)frame->r10;
    int      fd     = (int)frame->r8;
    uint64_t offset = frame->r9;

    if (length == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    if (offset & (PAGE_SIZE - 1)) { frame->rax = (uint64_t)-EINVAL; return; }

    // Guard against integer overflow in page-count calculation
    if (length > USER_ADDR_MAX) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t ignored_start;
    uint64_t ignored_end;
    // Validate addr if MAP_FIXED or hint provided, including page rounding.
    if (addr != 0 && !user_page_range_ok(addr, length, &ignored_start, &ignored_end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    bool fixed = (flags & MAP_FIXED) != 0;
    bool fixed_noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;
    if ((fixed || fixed_noreplace) && (addr & (PAGE_SIZE - 1))) { frame->rax = (uint64_t)-EINVAL; return; }
    bool anonymous = (flags & MAP_ANONYMOUS) != 0;
    // Prevent mapping the zero page (NULL dereference mitigation).
    // MAP_FIXED at address 0 is explicitly forbidden; anonymous hint 0
    // is allowed but allocator must not return 0 (handled below).
    if ((fixed || fixed_noreplace) && addr < PAGE_SIZE) { frame->rax = (uint64_t)-EPERM; return; }
    if (!anonymous && fixed && fd == -1) { frame->rax = (uint64_t)-EINVAL; return; }
    if (anonymous && fd != -1) { frame->rax = (uint64_t)-EINVAL; return; }

    // Reject W+X mappings (W^X policy)
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        frame->rax = (uint64_t)-EACCES; return;
    }

    uint64_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    // Overflow check: ensure num_pages * PAGE_SIZE doesn't wrap
    if (num_pages > (USER_ADDR_MAX / PAGE_SIZE)) { frame->rax = (uint64_t)-EINVAL; return; }
    uint64_t map_size = num_pages * PAGE_SIZE;
    if (addr != 0 && !user_page_range_ok(addr, map_size, &ignored_start, &ignored_end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if (!anonymous && offset > UINT64_MAX - map_size) { frame->rax = (uint64_t)-EINVAL; return; }
    bool requested_fb = false;
    struct limine_framebuffer *mapped_fb = NULL;
    fd_entry_t *mapping_entry = NULL;
    if (!anonymous) {
        mapping_entry = get_current_fd(fd);
        if (!mapping_entry) { frame->rax = (uint64_t)-EBADF; return; }
        if (!fd_allows_read(mapping_entry)) { frame->rax = (uint64_t)-EACCES; return; }
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !fd_allows_write(mapping_entry)) {
            frame->rax = (uint64_t)-EACCES;
            return;
        }
        char rel[256];
        if (mapping_entry->type == FD_DEV && is_devtmpfs_path(mapping_entry->path, rel) && rel[0] == 'f' && rel[1] == 'b' && rel[2] >= '0' && rel[2] <= '9' && rel[3] == '\0') {
            requested_fb = true;
            if (current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EACCES; return; }
            int idx = rel[2] - '0';
            if (!fb_req.response || idx >= (int)fb_req.response->framebuffer_count) { frame->rax = (uint64_t)-ENODEV; return; }
            mapped_fb = fb_req.response->framebuffers[idx];
            if (!mapped_fb || (mapped_fb->height && mapped_fb->pitch > UINT64_MAX / mapped_fb->height)) { frame->rax = (uint64_t)-EINVAL; return; }
            uint64_t fb_size = mapped_fb->height * mapped_fb->pitch;
            uint64_t available_pages = offset < fb_size ? (fb_size - offset + PAGE_SIZE - 1) / PAGE_SIZE : 0;
            if (num_pages > available_pages) { frame->rax = (uint64_t)-EINVAL; return; }
        }
        if (!requested_fb && mapping_entry->type != FD_FILE && mapping_entry->type != FD_TMPFS && mapping_entry->type != FD_EXT4 && mapping_entry->type != FD_ISO9660 && mapping_entry->type != FD_VFAT) {
            frame->rax = (uint64_t)-ENODEV;
            return;
        }
    }

    uint64_t replaced_pages = 0;
    if (fixed) {
        replaced_pages = flagged_vma_pages_in_range(&current_task_ptr->ctx->vmas, addr, addr + num_pages * PAGE_SIZE, VMA_FLAG_MMAP);
        if (replaced_pages > current_task_ptr->ctx->mmap_pages) replaced_pages = current_task_ptr->ctx->mmap_pages;
    }
    uint64_t retained_pages = current_task_ptr->ctx->mmap_pages - replaced_pages;
    if (num_pages > MAX_USER_MMAP_PAGES - retained_pages) {
        frame->rax = (uint64_t)-ENOMEM;
        return;
    }

    uint64_t vmm_flags = VMM_USER;
    if (prot & PROT_WRITE) vmm_flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_NX;
    if (flags & MAP_SHARED) vmm_flags |= VMM_SHARED;

    void *ptr = NULL;
    if (fixed_noreplace && user_range_is_mapped(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE)) {
        frame->rax = (uint64_t)-EEXIST;
        return;
    }
    if (fixed || fixed_noreplace) {
        // MAP_FIXED must *replace* any existing mapping in [addr, addr+size):
        // POSIX/Linux semantics are that the old mappings are discarded, not
        // merged.  ld.so relies on this when it overlays an anonymous
        // (zero-initialised) BSS-overflow map on top of an earlier file-backed
        // mapping of the same address range.  Without this unmap step, the
        // "already mapped, just update flags" branch in vmap_user_at silently
        // keeps the old *physical* page and its stale file bytes — so BSS lock
        // words end up holding garbage from the previously-mapped file
        // (observed: libc.so.6 BSS at runtime contained "inet..." ASCII bytes
        // from the file, breaking __lll_lock_wait_private which then sees
        // val != 0, writes 2, and deadlocks).
        uint64_t fixed_start = addr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t fixed_end   = (addr + num_pages * PAGE_SIZE + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        for (uint64_t a = fixed_start; a < fixed_end; a += PAGE_SIZE) {
            if (get_vmm_pte(current_task_ptr->ctx, a) & (VMM_PRESENT | VMM_DEMAND)) unmap_vmm(current_task_ptr->ctx, a);
        }
        current_task_ptr->ctx->mmap_pages = retained_pages;
        ptr = vmap_user_at(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE, vmm_flags);
    } else if ((flags & MAP_32BIT) && addr == 0) {
        ptr = vmap_user_range_32(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    } else if (addr != 0 && !user_range_is_mapped(current_task_ptr->ctx, addr, num_pages * PAGE_SIZE)) {
        ptr = vmap_user_at(current_task_ptr->ctx, addr & ~(PAGE_SIZE - 1), num_pages * PAGE_SIZE, vmm_flags);
        if (!ptr) ptr = vmap_user_range(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    } else {
        ptr = vmap_user_range(current_task_ptr->ctx, num_pages * PAGE_SIZE, vmm_flags);
    }

    if (!ptr) {
        frame->rax = (uint64_t)-ENOMEM; return;
    }
    current_task_ptr->ctx->mmap_pages = retained_pages + num_pages;

    bool fb_mapped = false;
    if (requested_fb && mapped_fb) {
        uint64_t phys_base = virt_to_phys((void *)mapped_fb->address);
        if ((phys_base & (PAGE_SIZE - 1)) || phys_base > UINT64_MAX - offset || phys_base + offset > UINT64_MAX - map_size) {
            rollback_mmap(ptr, num_pages, retained_pages);
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
        uint64_t map_flags = VMM_USER | VMM_PWT | VMM_PCD | VMM_NX | VMM_EXTERNAL;
        if (prot & PROT_WRITE) map_flags |= VMM_WRITABLE;

        for (uint64_t i = 0; i < num_pages; i++) {
            uint64_t vaddr = (uint64_t)ptr + i * PAGE_SIZE;
            if (get_vmm_phys(current_task_ptr->ctx, vaddr) != 0) unmap_vmm(current_task_ptr->ctx, vaddr);
            if (!map_vmm(current_task_ptr->ctx, vaddr, phys_base + offset + i * PAGE_SIZE, map_flags)) {
                rollback_mmap(ptr, num_pages, retained_pages);
                frame->rax = (uint64_t)-ENOMEM;
                return;
            }
        }
        fb_mapped = true;
    }

    // Record the VMA so /proc/<pid>/maps can describe this mapping.
    {
        int vprot = 0;
        if (prot & PROT_READ)  vprot |= VMA_PROT_READ;
        if (prot & PROT_WRITE) vprot |= VMA_PROT_WRITE;
        if (prot & PROT_EXEC)  vprot |= VMA_PROT_EXEC;
        int vflags = 0;
        if (flags & MAP_ANONYMOUS) vflags |= VMA_FLAG_ANON;
        if (flags & MAP_SHARED)    vflags |= VMA_FLAG_SHARED;
        const char *name = NULL;
        char namebuf[256];
        if (!anonymous) {
            strncpy(namebuf, mapping_entry->path, sizeof(namebuf) - 1);
            namebuf[sizeof(namebuf) - 1] = '\0';
            name = namebuf;
        }
        if (!add_vma(&current_task_ptr->ctx->vmas, (uint64_t)ptr, (uint64_t)ptr + num_pages * PAGE_SIZE, vprot, vflags | VMA_FLAG_MMAP, offset, name)) {
            rollback_mmap(ptr, num_pages, retained_pages);
            frame->rax = (uint64_t)-ENOMEM;
            return;
        }
    }

    // File-backed mapping: copy data if not anonymous
    if (!anonymous) {
        if (fb_mapped) { frame->rax = (uint64_t)ptr; return; }
        fd_entry_t *entry = mapping_entry;

        if (entry->type == FD_EXT4) {
            uint8_t *chunk = malloc(65536);
            if (!chunk) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-ENOMEM; return; }
            uint64_t copied = 0;
            uint64_t map_size = num_pages * PAGE_SIZE;
            while (copied < map_size) {
                uint64_t amount = map_size - copied;
                if (amount > 65536) amount = 65536;
                int64_t got = read_ext4(entry->path, chunk, amount, offset + copied);
                if (got < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)got; return; }
                if (got == 0) break;
                if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr + copied, chunk, (uint64_t)got) < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; }
                copied += (uint64_t)got;
                if ((uint64_t)got < amount) break;
            }
            free(chunk);
        }
        if (entry->type == FD_ISO9660) {
            uint8_t *chunk = malloc(65536);
            if (!chunk) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-ENOMEM; return; }
            uint64_t copied = 0;
            uint64_t map_size = num_pages * PAGE_SIZE;
            while (copied < map_size) {
                uint64_t amount = map_size - copied;
                if (amount > 65536) amount = 65536;
                int64_t got = read_iso9660(entry->path, chunk, amount, offset + copied);
                if (got < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)got; return; }
                if (got == 0) break;
                if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr + copied, chunk, (uint64_t)got) < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; }
                copied += (uint64_t)got;
                if ((uint64_t)got < amount) break;
            }
            free(chunk);
        }
        if (entry->type == FD_VFAT) {
            uint8_t *chunk = malloc(65536);
            if (!chunk) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-ENOMEM; return; }
            uint64_t copied = 0;
            uint64_t map_size = num_pages * PAGE_SIZE;
            while (copied < map_size) {
                uint64_t amount = map_size - copied;
                if (amount > 65536) amount = 65536;
                int64_t got = read_vfat(entry->path, chunk, amount, offset + copied);
                if (got < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)got; return; }
                if (got == 0) break;
                if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr + copied, chunk, (uint64_t)got) < 0) { free(chunk); rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; }
                copied += (uint64_t)got;
                if ((uint64_t)got < amount) break;
            }
            free(chunk);
        }
        if (entry->type == FD_TMPFS) {
            tmpfs_file_t file = read_tmpfs(entry->path);
            uint64_t map_size = num_pages * PAGE_SIZE;
            uint64_t file_avail = file.size > offset ? file.size - offset : 0;
            uint64_t copy_len = file_avail < map_size ? file_avail : map_size;
            if (copy_len && file.data) { if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr, (uint8_t *)file.data + offset, copy_len) < 0) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; } }
        } else {
            initrd_file_t file = read_initrd(entry->path);
            if (file.data) {
            // Validate offset is within file bounds to prevent out-of-bounds read
            if (offset >= file.size && file.size > 0) {
                // Nothing to copy, mapping is zero-filled
            } else {
                uint64_t map_size = num_pages * PAGE_SIZE;
                uint64_t file_avail = (file.size > offset) ? (file.size - offset) : 0;
                uint64_t copy_len = (file_avail < map_size) ? file_avail : map_size;
                if (copy_len > 0) { if (write_vmm(current_task_ptr->ctx, (uint64_t)ptr, (uint8_t *)file.data + offset, copy_len) < 0) { rollback_mmap(ptr, num_pages, retained_pages); frame->rax = (uint64_t)-EFAULT; return; } }
            }
            // Zero the remaining bytes (BSS-like) is already handled by vmap_user_at/vmalloc_ex
            // which zeroed the newly allocated pages.
            }
        }
    }

    frame->rax = (uint64_t)ptr;
}

void sys_mprotect(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;

    if (length == 0) { frame->rax = 0; return; }
    // Ensure entire range is in user-space
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        frame->rax = (uint64_t)-EACCES; return;
    }

    uint64_t start;
    uint64_t end;
    if (!user_page_range_ok(addr, length, &start, &end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t phys = get_vmm_phys(current_task_ptr->ctx, a);
        if (!phys) { frame->rax = (uint64_t)-ENOMEM; return; }
        uint64_t vmm_flags = VMM_USER;
        if (prot & PROT_WRITE) vmm_flags |= VMM_WRITABLE;
        if (!(prot & PROT_EXEC)) vmm_flags |= VMM_NX;
        map_vmm(current_task_ptr->ctx, a, phys, vmm_flags);
    }

    // Reflect the protection change in the VMA table.
    {
        int vprot = 0;
        if (prot & PROT_READ)  vprot |= VMA_PROT_READ;
        if (prot & PROT_WRITE) vprot |= VMA_PROT_WRITE;
        if (prot & PROT_EXEC)  vprot |= VMA_PROT_EXEC;
        protect_vma(&current_task_ptr->ctx->vmas, start, end, vprot);
    }

    frame->rax = 0;
}

void sys_munmap(syscall_frame_t *frame) {
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;

    if (length == 0) { frame->rax = (uint64_t)-EINVAL; return; }
    // Ensure entire range is in user-space
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)addr, length)) { frame->rax = (uint64_t)-EINVAL; return; }

    uint64_t start;
    uint64_t end;
    if (!user_page_range_ok(addr, length, &start, &end)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    uint64_t removed_pages = flagged_vma_pages_in_range(&current_task_ptr->ctx->vmas, start, end, VMA_FLAG_MMAP);
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        unmap_vmm(current_task_ptr->ctx, a);
    }

    if (removed_pages > current_task_ptr->ctx->mmap_pages) removed_pages = current_task_ptr->ctx->mmap_pages;
    current_task_ptr->ctx->mmap_pages -= removed_pages;

    // Drop the now-unmapped range from the VMA table.
    remove_vma(&current_task_ptr->ctx->vmas, start, end);

    frame->rax = 0;
}

void sys_brk(syscall_frame_t *frame) {
    uint64_t addr = frame->rdi;

    if (addr == 0) {
        // Return current break
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Validate: new brk must be in user-space and above brk_start
    if (!user_address_range_ok(addr, 1)) { frame->rax = current_task_ptr->brk; return; }
    if (addr < current_task_ptr->brk_start) {
        // Cannot shrink below initial heap start
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Enforce upper bound to prevent unconstrained heap growth
    if (addr - current_task_ptr->brk_start > MAX_BRK_SIZE) {
        frame->rax = current_task_ptr->brk;
        return;
    }

    // Align to page boundary
    uint64_t old_brk = current_task_ptr->brk & ~0xFFFULL;
    if (addr > UINT64_MAX - (PAGE_SIZE - 1)) {
        frame->rax = current_task_ptr->brk;
        return;
    }
    uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_brk > old_brk) {
        // Map new pages
        for (uint64_t a = old_brk; a < new_brk; a += 4096) {
            if (get_vmm_phys(current_task_ptr->ctx, a) == 0) {
                void *page = pmalloc();
                if (!page) {
                    for (uint64_t rollback = old_brk; rollback < a; rollback += PAGE_SIZE) unmap_vmm(current_task_ptr->ctx, rollback);
                    frame->rax = current_task_ptr->brk;
                    return;
                }
                if (!map_vmm(current_task_ptr->ctx, a, (uint64_t)page, VMM_USER | VMM_WRITABLE | VMM_NX)) {
                    pfree(page);
                    for (uint64_t rollback = old_brk; rollback < a; rollback += PAGE_SIZE) unmap_vmm(current_task_ptr->ctx, rollback);
                    frame->rax = current_task_ptr->brk;
                    return;
                }
                (void)memset_vmm(current_task_ptr->ctx, a, 0, 4096);
            }
        }
    }

    current_task_ptr->brk = addr;
    // Keep the [heap] VMA in sync with the break so /proc/<pid>/maps is accurate.
    set_vma_heap(&current_task_ptr->ctx->vmas, current_task_ptr->brk_start, current_task_ptr->brk);
    frame->rax = current_task_ptr->brk;
}
