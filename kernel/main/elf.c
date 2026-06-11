#include <main/elf.h>
#include <main/rootfs.h>
#include <main/devfs.h>
#include <freestanding/errno.h>
#include <main/string.h>
#include <io/terminal.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <freestanding/sys/types.h>
#include <main/spinlock.h>
#include <syscalls/syscalls.h>
#include <io/hpet.h>


// Simple entropy source using HPET counter
static uint64_t aslr_entropy_counter = 0;

static uint64_t aslr_random_offset(uint64_t max_pages) {
    // Mix HPET counter with a simple counter for entropy
    uint64_t hpet = read_hpet_counter();
    aslr_entropy_counter += 0x9E3779B97F4A7C15ULL; // golden ratio constant
    uint64_t mixed = hpet ^ aslr_entropy_counter;
    // Simple xorshift
    mixed ^= mixed << 13;
    mixed ^= mixed >> 7;
    mixed ^= mixed << 17;
    return (mixed % max_pages) * PAGE_SIZE;
}

static uint64_t setup_stack(vmm_context_t *ctx, uint64_t v_rsp,
                             char **argv, char **envp, elf64_auxv_t *auxv) {
    // Count argv and envp
    int argc = 0;
    if (argv) while (argv[argc]) argc++;
    int envc = 0;
    if (envp) while (envp[envc]) envc++;

    if (argc > 64) argc = 64;
    if (envc > 64) envc = 64;

    uint64_t arg_ptrs[64];
    uint64_t env_ptrs[64];

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

    // Push auxv (terminated by AT_NULL)
    if (auxv) {
        int auxc = 0;
        while (auxv[auxc].type != AT_NULL) auxc++;
        auxc++; // include AT_NULL
        v_rsp -= auxc * sizeof(elf64_auxv_t);
        write_vmm(ctx, v_rsp, auxv, auxc * sizeof(elf64_auxv_t));
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

static int load_elf_segments(vmm_context_t *ctx, uint8_t *data,
                              elf64_ehdr_t *ehdr, uint64_t base_addr,
                              elf64_dyn_t **dynamic_out) {
    elf64_phdr_t *phdrs = (elf64_phdr_t *)(data + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];

        if (ph->type == PT_LOAD) {
            uint64_t seg_start = base_addr + ph->vaddr;
            uint64_t seg_end   = seg_start + ph->memsz;
            uint64_t final_flags = VMM_USER;
            if (ph->flags & PF_W) final_flags |= VMM_WRITABLE;
            if (!(ph->flags & PF_X)) final_flags |= VMM_NX;

            uint64_t page_start = seg_start & ~0xFFFULL;
            uint64_t page_end   = (seg_end + 0xFFFULL) & ~0xFFFULL;

            for (uint64_t a = page_start; a < page_end; a += 0x1000) {
                if (get_vmm_phys(ctx, a) == 0) {
                    void *page = pmalloc();
                    map_vmm(ctx, a, (uint64_t)page, VMM_USER | VMM_WRITABLE | VMM_NX);
                    memset_vmm(ctx, a, 0, 0x1000);
                }
            }

            if (ph->filesz > 0) {
                for (uint64_t i = 0; i < ph->filesz; i++) {
                    uint8_t byte = data[ph->offset + i];
                    write_vmm(ctx, seg_start + i, &byte, 1);
                }
            }

            if (ph->memsz > ph->filesz) {
                uint64_t bss_start = seg_start + ph->filesz;
                uint64_t bss_size  = ph->memsz - ph->filesz;

                uint8_t zero = 0;
                for (uint64_t i = 0; i < bss_size; i++) {
                    write_vmm(ctx, bss_start + i, &zero, 1);
                }
            }

            for (uint64_t a = page_start; a < page_end; a += 0x1000) {
                uint64_t phys = get_vmm_phys(ctx, a);
                if (phys) map_vmm(ctx, a, phys, final_flags);
            }

        } else if (ph->type == PT_DYNAMIC && dynamic_out) {
            *dynamic_out = (elf64_dyn_t *)(base_addr + ph->vaddr);
        }
    }

    return 0;
}

pid_t execute_elf(const char *path, char **argv, char **envp) {
    if (devfs_device_exists(path)) return -EACCES;

    rootfs_file_t file = read_rootfs(path);
    if (!file.data) return -ENOENT;

    if (file.mode & 0x4000) return -EISDIR;
    if (!(file.mode & 0111)) return -EPERM;

    uint8_t *data = (uint8_t *)file.data;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (ehdr->magic != ELF_MAGIC || ehdr->class != ELF_CLASS64 || ehdr->machine != EM_X86_64) return -ENOEXEC;

    vmm_context_t *ctx = create_vmm_context();
    if (!ctx) return -ENOMEM;

    // ASLR: randomize PIE base address
    uint64_t base_addr = (ehdr->type == ET_DYN) ? (0x100000000ULL + aslr_random_offset(0x40000)) : 0;
    const char *interp_path = NULL;

    elf64_phdr_t *phdrs = (elf64_phdr_t *)(data + ehdr->phoff);
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_INTERP) {
            interp_path = (const char *)(data + phdrs[i].offset);
        }
    }

    load_elf_segments(ctx, data, ehdr, base_addr, NULL);

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
        rootfs_file_t ifile = read_rootfs(interp_path);
        if (ifile.data) {
            uint8_t *idata = (uint8_t *)ifile.data;
            elf64_ehdr_t *iehdr = (elf64_ehdr_t *)idata;
            // ASLR: randomize interpreter base
            interp_base = 0x5000000000ULL + aslr_random_offset(0x40000);
            load_elf_segments(ctx, idata, iehdr, interp_base, NULL);
            entry = iehdr->entry + interp_base;
        }
    }

    elf64_auxv_t auxv[10];
    int a = 0;
    auxv[a++] = (elf64_auxv_t){AT_PHDR, {.val = phdr_addr}};
    auxv[a++] = (elf64_auxv_t){AT_PHNUM, {.val = ehdr->phnum}};
    auxv[a++] = (elf64_auxv_t){AT_PHENT, {.val = ehdr->phentsize}};
    auxv[a++] = (elf64_auxv_t){AT_ENTRY, {.val = ehdr->entry + base_addr}};
    auxv[a++] = (elf64_auxv_t){AT_BASE, {.val = interp_base}};
    auxv[a++] = (elf64_auxv_t){AT_PAGESZ, {.val = 4096}};
    auxv[a++] = (elf64_auxv_t){AT_RANDOM, {.val = aslr_random_offset(1)}};
    auxv[a++] = (elf64_auxv_t){AT_NULL, {.val = 0}};

    void *stack = vmalloc_user_ex(ctx, USER_STACK_SIZE);
    if (!stack) return -ENOMEM;

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
    if (pid >= 0) {
        tasks[pid].stack_base = stack;
        tasks[pid].brk = heap_start;
        tasks[pid].brk_start = heap_start;
    }

    return pid;
}

int execve_elf(const char *path, char **argv, char **envp, void* raw_frame) {
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;

    if (devfs_device_exists(path))
        return -EACCES;

    rootfs_file_t file = read_rootfs(path);
    if (!file.data) return -ENOENT;

    if (file.mode & 0x4000) return -EISDIR;
    if (!(file.mode & 0111)) return -EPERM;

    uint8_t *data = (uint8_t *)file.data;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (ehdr->magic != ELF_MAGIC || ehdr->class != ELF_CLASS64 || ehdr->machine != EM_X86_64)
        return -ENOEXEC;

    vmm_context_t *ctx = create_vmm_context();
    if (!ctx) return -ENOMEM;

    // ASLR: randomize PIE base address
    uint64_t base_addr = (ehdr->type == ET_DYN) ? (0x100000000ULL + aslr_random_offset(0x40000)) : 0;
    const char *interp_path = NULL;

    elf64_phdr_t *phdrs = (elf64_phdr_t *)(data + ehdr->phoff);
    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_INTERP) {
            interp_path = (const char *)(data + phdrs[i].offset);
        }
    }

    load_elf_segments(ctx, data, ehdr, base_addr, NULL);

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
        rootfs_file_t ifile = read_rootfs(interp_path);
        if (ifile.data) {
            uint8_t *idata = (uint8_t *)ifile.data;
            elf64_ehdr_t *iehdr = (elf64_ehdr_t *)idata;
            // ASLR: randomize interpreter base
            interp_base = 0x5000000000ULL + aslr_random_offset(0x40000);
            load_elf_segments(ctx, idata, iehdr, interp_base, NULL);
            entry = iehdr->entry + interp_base;
        }
    }

    elf64_auxv_t auxv[10];
    int a = 0;
    auxv[a++] = (elf64_auxv_t){AT_PHDR, {.val = phdr_addr}};
    auxv[a++] = (elf64_auxv_t){AT_PHNUM, {.val = ehdr->phnum}};
    auxv[a++] = (elf64_auxv_t){AT_PHENT, {.val = ehdr->phentsize}};
    auxv[a++] = (elf64_auxv_t){AT_ENTRY, {.val = ehdr->entry + base_addr}};
    auxv[a++] = (elf64_auxv_t){AT_BASE, {.val = interp_base}};
    auxv[a++] = (elf64_auxv_t){AT_PAGESZ, {.val = 4096}};
    auxv[a++] = (elf64_auxv_t){AT_RANDOM, {.val = aslr_random_offset(1)}};
    auxv[a++] = (elf64_auxv_t){AT_NULL, {.val = 0}};

    void *stack = vmalloc_user_ex(ctx, USER_STACK_SIZE);
    if (!stack) return -ENOMEM;

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

    current_task_ptr->ctx = ctx;
    current_task_ptr->stack_base = stack;
    switch_vmm_context(ctx);

    frame->rcx = entry;
    frame->r12 = v_rsp;
    return 0;
}
