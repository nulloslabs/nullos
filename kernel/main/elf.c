#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <main/elf.h>
#include <main/msr.h>
#include <main/spinlocks.h>
#include <main/rng.h>
#include <main/string.h>
#include <io/initrd.h>
#include <io/devtmpfs.h>
#include <io/procfs.h>
#include <io/vfs.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <syscalls/syscalls.h>

#define ELF_USER_ADDR_MAX 0x0000800000000000ULL
#define ELF_MAX_PHNUM 128

static bool elf_range_valid(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

static bool elf_add_valid(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return false;
    *out = a + b;
    return true;
}

static int validate_elf_image(const uint8_t *data, uint64_t size, uint64_t base_addr, const elf64_ehdr_t **ehdr_out) {
    if (!data || size < sizeof(elf64_ehdr_t)) return -ENOEXEC;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)data;
    if (ehdr->magic != ELF_MAGIC || ehdr->class != ELF_CLASS64 || ehdr->data != ELF_DATA2LSB || ehdr->machine != EM_X86_64) return -ENOEXEC;
    if (ehdr->type != ET_EXEC && ehdr->type != ET_DYN) return -ENOEXEC;
    if (ehdr->ehsize < sizeof(*ehdr) || ehdr->phentsize != sizeof(elf64_phdr_t) || ehdr->phnum == 0 || ehdr->phnum > ELF_MAX_PHNUM) return -ENOEXEC;

    uint64_t phdr_size = (uint64_t)ehdr->phnum * sizeof(elf64_phdr_t);
    if (!elf_range_valid(ehdr->phoff, phdr_size, size)) return -ENOEXEC;

    uint64_t entry;
    if (!elf_add_valid(base_addr, ehdr->entry, &entry) || entry >= ELF_USER_ADDR_MAX) return -ENOEXEC;

    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(data + ehdr->phoff);
    bool entry_is_executable = false;
    bool has_load = false;
    for (uint16_t i = 0; i < ehdr->phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->type == PT_INTERP) {
            if (ph->filesz < 2 || ph->filesz > 256 || !elf_range_valid(ph->offset, ph->filesz, size)) return -ENOEXEC;
            if (data[ph->offset + ph->filesz - 1] != '\0') return -ENOEXEC;
            continue;
        }
        if (ph->type != PT_LOAD) continue;
        has_load = true;
        if (ph->filesz > ph->memsz || !elf_range_valid(ph->offset, ph->filesz, size)) return -ENOEXEC;
        if ((ph->flags & PF_W) && (ph->flags & PF_X)) return -ENOEXEC;

        uint64_t start;
        uint64_t end;
        if (!elf_add_valid(base_addr, ph->vaddr, &start) || !elf_add_valid(start, ph->memsz, &end)) return -ENOEXEC;
        if (start >= ELF_USER_ADDR_MAX || end > ELF_USER_ADDR_MAX || end < start) return -ENOEXEC;
        if ((ph->offset & (PAGE_SIZE - 1)) != (ph->vaddr & (PAGE_SIZE - 1))) return -ENOEXEC;
        if ((ph->flags & PF_X) && entry >= start && entry < end) entry_is_executable = true;
    }

    if (!has_load || !entry_is_executable) return -ENOEXEC;
    if (ehdr_out) *ehdr_out = ehdr;
    return 0;
}

static bool can_execute_file(const initrd_file_t *file) {
    if (!file || !current_task_ptr) return false;
    if (current_task_ptr->euid == 0) return (file->mode & 0111) != 0;
    int shift = current_task_ptr->euid == file->uid ? 6 : (current_task_ptr->egid == file->gid ? 3 : 0);
    return ((file->mode >> shift) & 1) != 0;
}

static uint64_t aslr_random_offset(uint64_t max_pages) {
    uint64_t val;
    get_random_bytes(&val, sizeof(val));
    return (val % max_pages) * PAGE_SIZE;
}

// Set the task's thread name from the basename of its exe path
// (mirrors Linux's PR_SET_NAME default of the executable's basename).
static void set_task_name_from_path(task_t *t, const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    strncpy(t->name, base, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
}

static uint64_t setup_stack(vmm_context_t *ctx, uint64_t v_rsp, char **argv, char **envp, elf64_auxv_t *auxv) {
    // Count argv and envp
    int argc = 0;
    if (argv) while (argv[argc]) argc++;
    int envc = 0;
    if (envp) while (envp[envc]) envc++;

    if (argc > 64) argc = 64;
    if (envc > 64) envc = 64;

    uint64_t arg_ptrs[64];
    uint64_t env_ptrs[64];
    uint8_t rand_bytes[16];

    get_random_bytes(rand_bytes, sizeof(rand_bytes));
    v_rsp -= sizeof(rand_bytes);
    write_vmm(ctx, v_rsp, rand_bytes, sizeof(rand_bytes));
    if (auxv) {
        for (int i = 0; auxv[i].type != AT_NULL; i++) {
            if (auxv[i].type == AT_RANDOM) {
                auxv[i].un.val = v_rsp;
                break;
            }
        }
    }

    // Push env strings
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        v_rsp -= len;
        write_vmm(ctx, v_rsp, envp[i], len);
        env_ptrs[i] = v_rsp;
    }

    // Push argv strings
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        v_rsp -= len;
        write_vmm(ctx, v_rsp, argv[i], len);
        arg_ptrs[i] = v_rsp;
    }

    v_rsp &= ~0xFULL;

    uint64_t ptr_area_size = 8ULL * (1 + argc + 1 + envc + 1);
    size_t aux_size = 0;
    if (auxv) {
        int auxc = 0;
        while (auxv[auxc].type != AT_NULL) auxc++;
        auxc++; // include AT_NULL
        aux_size = auxc * sizeof(elf64_auxv_t);
    }

    if ((v_rsp - aux_size - ptr_area_size) & 0xFULL) {
        v_rsp -= 8;
    }

    // Push auxv (terminated by AT_NULL). Nothing may be placed between envp's
    // NULL terminator and auxv; the loader finds auxv by walking past envp.
    if (auxv) {
        int auxc = 0;
        while (auxv[auxc].type != AT_NULL) auxc++;
        auxc++; // include AT_NULL
        v_rsp -= aux_size;
        write_vmm(ctx, v_rsp, auxv, aux_size);
    }

    uint64_t zero = 0;

    // Push envp array (NULL terminated)
    v_rsp -= 8; write_vmm(ctx, v_rsp, &zero, 8);
    for (int i = envc - 1; i >= 0; i--) {
        v_rsp -= 8;
        write_vmm(ctx, v_rsp, &env_ptrs[i], 8);
    }

    // Push argv array (NULL terminated)
    v_rsp -= 8; write_vmm(ctx, v_rsp, &zero, 8);
    for (int i = argc - 1; i >= 0; i--) {
        v_rsp -= 8;
        write_vmm(ctx, v_rsp, &arg_ptrs[i], 8);
    }

    // Push argc
    uint64_t argc_u64 = argc;
    v_rsp -= 8;
    write_vmm(ctx, v_rsp, &argc_u64, 8);
    return v_rsp;
}

static int load_elf_segments(vmm_context_t *ctx, const uint8_t *data, const elf64_ehdr_t *ehdr, uint64_t base_addr, elf64_dyn_t **dynamic_out, vma_table_t *vmas, const char *name) {
    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(data + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];

        if (ph->type == PT_LOAD) {
            uint64_t seg_start = base_addr + ph->vaddr;
            uint64_t seg_end   = seg_start + ph->memsz;
            uint64_t final_flags = VMM_USER;
            if (ph->flags & PF_W) final_flags |= VMM_WRITABLE;
            if (!(ph->flags & PF_X)) final_flags |= VMM_NX;

            uint64_t page_start = seg_start & ~0xFFFULL;
            uint64_t page_end   = (seg_end + 0xFFFULL) & ~0xFFFULL;

            // 1) Allocate all pages for this PT_LOAD segment up front.
            //    pmalloc() already returns zeroed pages (defensive: the kernel
            //    has no destroy_vmm_context path, so recycled pages can hold
            //    stale lock words). Only memset again if pmalloc didn't run
            //    for a page that was already mapped by an earlier segment.
            for (uint64_t a = page_start; a < page_end; a += 0x1000) {
                if (get_vmm_phys(ctx, a) == 0) {
                    void *page = pmalloc();
                    if (!page) return -ENOMEM;
                    if (!map_vmm(ctx, a, (uint64_t)page, VMM_USER | VMM_WRITABLE | VMM_NX)) {
                        pfree(page);
                        return -ENOMEM;
                    }
                }
            }

            // 2) Bulk-copy the segment file image. The old code did
            //    write_vmm(ctx, addr, &byte, 1) per byte — a full page-table
            //    walk for EVERY byte. For a 2MB glibc that's ~2M walks.
            //    Walk the page table once per page and memcpy the bytes that
            //    land in each page directly via the HHDM alias.
            if (ph->filesz > 0) {
                uint64_t dst = seg_start;
                uint64_t left = ph->filesz;
                const uint8_t *src = data + ph->offset;
                while (left > 0) {
                    uint64_t phys = get_vmm_phys(ctx, dst & ~0xFFFULL);
                    if (!phys) break;
                    uint64_t off = dst & 0xFFF;
                    uint64_t chunk = 4096 - off;
                    if (chunk > left) chunk = left;
                    memcpy((uint8_t *)phys_to_virt(phys) + off, src, chunk);
                    src  += chunk;
                    dst  += chunk;
                    left -= chunk;
                }
            }

            // 3) Zero the BSS region in page-sized chunks. The individual
            //    data pages were already zeroed by pmalloc(); only the tail
            //    of a partially-written page needs re-zeroing here. Still,
            //    a page-at-a-time memset is O(thousands) faster than the old
            //    byte-at-a-time write_vmm ZSS loop.
            if (ph->memsz > ph->filesz) {
                uint64_t bss_start = seg_start + ph->filesz;
                uint64_t bss_end   = seg_start + ph->memsz;
                uint64_t a = bss_start;
                while (a < bss_end) {
                    uint64_t phys = get_vmm_phys(ctx, a & ~0xFFFULL);
                    if (!phys) break;
                    uint64_t off = a & 0xFFF;
                    uint64_t chunk = 4096 - off;
                    uint64_t remain = bss_end - a;
                    if (chunk > remain) chunk = remain;
                    memset((uint8_t *)phys_to_virt(phys) + off, 0, chunk);
                    a += chunk;
                }
            }

            for (uint64_t a = page_start; a < page_end; a += 0x1000) {
                uint64_t phys = get_vmm_phys(ctx, a);
                if (phys) map_vmm(ctx, a, phys, final_flags);
            }

            // Record this PT_LOAD segment as a VMA for /proc/<pid>/maps.
            if (vmas) {
                int vprot = VMA_PROT_READ;  // PT_LOAD is always readable
                if (ph->flags & PF_W) vprot |= VMA_PROT_WRITE;
                if (ph->flags & PF_X) vprot |= VMA_PROT_EXEC;
                add_vma(vmas, page_start, page_end, vprot, 0, ph->offset, name);
            }

        } else if (ph->type == PT_DYNAMIC && dynamic_out) {
            *dynamic_out = (elf64_dyn_t *)(base_addr + ph->vaddr);
        }
    }

    return 0;
}

int execute_elf(const char *path, char **argv, char **envp) {
    if (check_vfs_device(path)) return -EACCES;

    initrd_file_t file = read_initrd(path);
    if (!file.data) return -ENOENT;

    if (S_ISDIR(file.mode)) return -EISDIR;
    if (!can_execute_file(&file)) return -EACCES;

    uint8_t *data = (uint8_t *)file.data;
    const elf64_ehdr_t *ehdr = NULL;
    uint64_t base_addr = 0;
    if (file.size >= sizeof(elf64_ehdr_t) && ((elf64_ehdr_t *)data)->type == ET_DYN) base_addr = 0x100000000ULL + aslr_random_offset(0x40000);
    int validation = validate_elf_image(data, file.size, base_addr, &ehdr);
    if (validation < 0) return validation;

    vmm_context_t *ctx = create_vmm_context();
    if (!ctx) return -ENOMEM;

    // ASLR: randomize PIE base address
    const char *interp_path = NULL;

    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(data + ehdr->phoff);
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_INTERP) {
            interp_path = (const char *)(data + phdrs[i].offset);
        }
    }

    // Record VMAs before the task exists, then attach them after creation.
    vma_table_t local_vmas;
    init_vma_table(&local_vmas);

    int load_status = load_elf_segments(ctx, data, ehdr, base_addr, NULL, &local_vmas, path);
    if (load_status < 0) { destroy_vmm_context(ctx); return load_status; }

    uint64_t phdr_addr = 0;
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_LOAD && phdrs[i].offset == 0) {
            phdr_addr = base_addr + phdrs[i].vaddr + ehdr->phoff;
            break;
        }
    }

    uint64_t entry = ehdr->entry + base_addr;
    uint64_t interp_base = 0;

    if (interp_path) {
        initrd_file_t ifile = read_initrd(interp_path);
        if (ifile.data) {
            uint8_t *idata = (uint8_t *)ifile.data;
            const elf64_ehdr_t *iehdr = NULL;
            if (ifile.size >= sizeof(elf64_ehdr_t) && ((elf64_ehdr_t *)idata)->type == ET_DYN) interp_base = 0x5000000000ULL + aslr_random_offset(0x40000);
            validation = validate_elf_image(idata, ifile.size, interp_base, &iehdr);
            if (validation < 0) { destroy_vmm_context(ctx); return validation; }
            load_status = load_elf_segments(ctx, idata, iehdr, interp_base, NULL, &local_vmas, interp_path);
            if (load_status < 0) { destroy_vmm_context(ctx); return load_status; }
            entry = iehdr->entry + interp_base;
        } else { destroy_vmm_context(ctx); return -ENOENT; }
    }

    elf64_auxv_t auxv[10];
    int a = 0;
    auxv[a++] = (elf64_auxv_t){AT_PHDR, {.val = phdr_addr}};
    auxv[a++] = (elf64_auxv_t){AT_PHNUM, {.val = ehdr->phnum}};
    auxv[a++] = (elf64_auxv_t){AT_PHENT, {.val = ehdr->phentsize}};
    auxv[a++] = (elf64_auxv_t){AT_ENTRY, {.val = ehdr->entry + base_addr}};
    auxv[a++] = (elf64_auxv_t){AT_BASE, {.val = interp_base}};
    auxv[a++] = (elf64_auxv_t){AT_PAGESZ, {.val = 4096}};
    auxv[a++] = (elf64_auxv_t){AT_RANDOM, {.val = 0}};
    auxv[a++] = (elf64_auxv_t){AT_NULL, {.val = 0}};

    uint64_t stack_vaddr = USER_STACK_BASE - USER_STACK_SIZE;
    void *stack = vmap_user_at(ctx, stack_vaddr, USER_STACK_SIZE, VMM_USER | VMM_WRITABLE | VMM_NX);
    if (!stack) { destroy_vmm_context(ctx); return -ENOMEM; }
    add_vma(&local_vmas, stack_vaddr, stack_vaddr + USER_STACK_SIZE,
            VMA_PROT_READ | VMA_PROT_WRITE, VMA_FLAG_ANON | VMA_FLAG_STACK, 0, "[stack]");

    char *empty_envp[] = { NULL };
    char **actual_envp = envp ? envp : empty_envp;

    uint64_t v_rsp = setup_stack(ctx, (uint64_t)stack + USER_STACK_SIZE - 8, argv, actual_envp, auxv);

    // Find highest loaded address
    uint64_t heap_start = 0;
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_LOAD) {
            uint64_t end = base_addr + phdrs[i].vaddr + phdrs[i].memsz;
            if (end > heap_start) heap_start = end;
        }
    }
    heap_start = (heap_start + 0xFFF) & ~0xFFFULL; // page align

    pid_t pid = create_task((void(*)(void))entry, 3, ctx, v_rsp);
    if (pid < 0) {
        destroy_vmm_context(ctx);
        return pid;
    }
    task_t *task = task_by_pid(pid);
    if (task) {
        task->stack_base = stack;
        task->brk = heap_start;
        task->brk_start = heap_start;
        strncpy(task->exe, path, sizeof(task->exe) - 1);
        task->exe[sizeof(task->exe) - 1] = '\0';
        set_task_name_from_path(task, path);
        // Hand off the VMA table we accumulated during loading.
        memcpy(&task->vmas, &local_vmas, sizeof(vma_table_t));
        // Save auxv for /proc/<pid>/auxv
        {
            int auxc = 0;
            while (auxv[auxc].type != AT_NULL) auxc++;
            auxc++; // include AT_NULL
            int words = auxc * 2;
            if (words > 16) words = 16;
            memcpy(task->auxv_blob, auxv, words * sizeof(uint64_t));
            task->auxv_blob_words = words;
        }
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->waiting_for == pid) {
            tasks[i]->waiting_for = 0;
            tasks[i]->state = TASK_READY;
            break;
        }
    }

    return 0;
}

int execve_elf(const char *path, char **argv, char **envp, void* raw_frame) {
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;

    if (check_vfs_device(path)) return -EACCES;
    if (is_procfs_path(path)) return -EACCES;

    initrd_file_t file = read_initrd(path);
    if (!file.data) return -ENOENT;

    if (S_ISDIR(file.mode)) return -EISDIR;
    if (!can_execute_file(&file)) return -EACCES;

    uint8_t *data = (uint8_t *)file.data;
    const elf64_ehdr_t *ehdr = NULL;
    uint64_t base_addr = 0;
    if (file.size >= sizeof(elf64_ehdr_t) && ((elf64_ehdr_t *)data)->type == ET_DYN) base_addr = 0x100000000ULL + aslr_random_offset(0x40000);
    int validation = validate_elf_image(data, file.size, base_addr, &ehdr);
    if (validation < 0) return validation;

    vmm_context_t *ctx = create_vmm_context();
    if (!ctx) return -ENOMEM;

    // ASLR: randomize PIE base address
    const char *interp_path = NULL;

    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(data + ehdr->phoff);
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_INTERP) {
            interp_path = (const char *)(data + phdrs[i].offset);
        }
    }

    // On exec, the old address space is replaced; reset the VMA table and
    // repopulate it as we load the new image.
    vma_table_t local_vmas;
    init_vma_table(&local_vmas);

    int load_status = load_elf_segments(ctx, data, ehdr, base_addr, NULL, &local_vmas, path);
    if (load_status < 0) { destroy_vmm_context(ctx); return load_status; }

    uint64_t phdr_addr = 0;
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_LOAD && phdrs[i].offset == 0) {
            phdr_addr = base_addr + phdrs[i].vaddr + ehdr->phoff;
            break;
        }
    }

    uint64_t entry = ehdr->entry + base_addr;
    uint64_t interp_base = 0;

    if (interp_path) {
        initrd_file_t ifile = read_initrd(interp_path);
        if (ifile.data) {
            uint8_t *idata = (uint8_t *)ifile.data;
            const elf64_ehdr_t *iehdr = NULL;
            if (ifile.size >= sizeof(elf64_ehdr_t) && ((elf64_ehdr_t *)idata)->type == ET_DYN) interp_base = 0x5000000000ULL + aslr_random_offset(0x40000);
            validation = validate_elf_image(idata, ifile.size, interp_base, &iehdr);
            if (validation < 0) { destroy_vmm_context(ctx); return validation; }
            load_status = load_elf_segments(ctx, idata, iehdr, interp_base, NULL, &local_vmas, interp_path);
            if (load_status < 0) { destroy_vmm_context(ctx); return load_status; }
            entry = iehdr->entry + interp_base;
        } else { destroy_vmm_context(ctx); return -ENOENT; }
    }

    elf64_auxv_t auxv[10];
    int a = 0;
    auxv[a++] = (elf64_auxv_t){AT_PHDR, {.val = phdr_addr}};
    auxv[a++] = (elf64_auxv_t){AT_PHNUM, {.val = ehdr->phnum}};
    auxv[a++] = (elf64_auxv_t){AT_PHENT, {.val = ehdr->phentsize}};
    auxv[a++] = (elf64_auxv_t){AT_ENTRY, {.val = ehdr->entry + base_addr}};
    auxv[a++] = (elf64_auxv_t){AT_BASE, {.val = interp_base}};
    auxv[a++] = (elf64_auxv_t){AT_PAGESZ, {.val = 4096}};
    auxv[a++] = (elf64_auxv_t){AT_RANDOM, {.val = 0}};
    auxv[a++] = (elf64_auxv_t){AT_NULL, {.val = 0}};

    uint64_t stack_vaddr = USER_STACK_BASE - USER_STACK_SIZE;
    void *stack = vmap_user_at(ctx, stack_vaddr, USER_STACK_SIZE, VMM_USER | VMM_WRITABLE | VMM_NX);
    if (!stack) { destroy_vmm_context(ctx); return -ENOMEM; }
    add_vma(&local_vmas, stack_vaddr, stack_vaddr + USER_STACK_SIZE, VMA_PROT_READ | VMA_PROT_WRITE, VMA_FLAG_ANON | VMA_FLAG_STACK, 0, "[stack]");

    char *empty_envp[] = { NULL };
    char **actual_envp = (envp) ? envp : empty_envp;

    uint64_t v_rsp = setup_stack(ctx, (uint64_t)stack + USER_STACK_SIZE - 8, argv, actual_envp, auxv);

    // Find highest loaded address
    uint64_t heap_start = 0;
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_LOAD) {
            uint64_t end = base_addr + phdrs[i].vaddr + phdrs[i].memsz;
            if (end > heap_start) heap_start = end;
        }
    }
    heap_start = (heap_start + 0xFFF) & ~0xFFFULL; // page align
    current_task_ptr->brk = heap_start;
    current_task_ptr->brk_start = heap_start;

    // Record the new exe path and splice in the freshly-built VMA table.
    strncpy(current_task_ptr->exe, path, sizeof(current_task_ptr->exe) - 1);
    current_task_ptr->exe[sizeof(current_task_ptr->exe) - 1] = '\0';
    set_task_name_from_path(current_task_ptr, path);
    memcpy(&current_task_ptr->vmas, &local_vmas, sizeof(vma_table_t));
    // Save auxv for /proc/<pid>/auxv
    {
        int auxc = 0;
        while (auxv[auxc].type != AT_NULL) auxc++;
        auxc++; // include AT_NULL
        int words = auxc * 2;
        if (words > 16) words = 16;
        memcpy(current_task_ptr->auxv_blob, auxv, words * sizeof(uint64_t));
        current_task_ptr->auxv_blob_words = words;
    }

    current_task_ptr->ctx = ctx;
    current_task_ptr->stack_base = stack;
    current_task_ptr->fs_base = 0;
    write_msr(MSR_FS_BASE, 0);
    switch_vmm_context(ctx);

    frame->rip = entry;
    frame->rdx = 0;
    frame->rsp = v_rsp;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->waiting_for == current_task_ptr->pid) {
            tasks[i]->waiting_for = 0;
            tasks[i]->state = TASK_READY;
            break;
        }
    }

    return 0;
}
