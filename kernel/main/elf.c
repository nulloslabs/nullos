#include <main/elf.h>
#include <main/rootfs.h>
#include <main/devfs.h>
#include <main/errno.h>
#include <main/string.h>
#include <io/terminal.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <freestanding/sys/types.h>
#include <main/spinlock.h>
#include <syscalls/syscalls.h>

static const char *so_search_paths[] = {
    "/usr/lib/",
    "/lib/",
    "/usr/local/lib/",
    NULL
};

static loaded_so_t *loaded_libraries = NULL;
static uint64_t next_so_base = 0x4000000000ULL;
static spinlock_t elf_lock = SPINLOCK_INIT;

static const char *strip_relative(const char *soname) {
    while (soname[0] == '.' && soname[1] == '.') {
        const char *slash = strchr(soname, '/');
        if (!slash) break;
        soname = slash + 1;
    }
    return soname;
}

static void read_vmm_str(vmm_context_t *ctx, uint64_t addr, char *buf, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        read_vmm(ctx, &buf[i], addr + i, 1);
        if (buf[i] == '\0') break;
    }
    buf[i] = '\0';
}

static uint64_t resolve_symbol(const char *name, vmm_context_t *ctx) {
    uint64_t irq;
    spin_lock_irqsave(&elf_lock, &irq);
    for (loaded_so_t *so = loaded_libraries; so; so = so->next) {
        if (!so->symtab || !so->strtab) continue;

        for (int i = 0; i < 5000; i++) {
            elf64_sym_t sym;
            read_vmm(ctx, &sym, (uint64_t)so->symtab + (i * sizeof(elf64_sym_t)), sizeof(elf64_sym_t));

            if (sym.name == 0 && sym.value == 0 && sym.info == 0 && i > 0) {
                if (i > 2000) break;
            }

            if (sym.name && sym.value != 0 && sym.shndx != 0) {
                char sym_name[128];
                read_vmm_str(ctx, (uint64_t)so->strtab + sym.name, sym_name, sizeof(sym_name));
                if (strcmp(sym_name, name) == 0) {
                    uint64_t res = so->base + sym.value;
                    spin_unlock_irqrestore(&elf_lock, irq);
                    return res;
                }
            }
        }
    }
    spin_unlock_irqrestore(&elf_lock, irq);
    return 0;
}

static int process_relocations(vmm_context_t *ctx, uint64_t base_addr,
                                elf64_rela_t *rela, uint64_t relasz,
                                elf64_rela_t *jmprel, uint64_t pltrelsz,
                                elf64_sym_t *symtab, char *strtab) {
    #define PROC_RELA(table, count) \
        for (uint64_t _i = 0; _i < (count); _i++) { \
            elf64_rela_t r; \
            read_vmm(ctx, &r, (uint64_t)(table) + (_i * sizeof(elf64_rela_t)), sizeof(elf64_rela_t)); \
            uint64_t target = base_addr + r.offset; \
            uint32_t type = ELF64_R_TYPE(r.info); \
            uint32_t sidx = ELF64_R_SYM(r.info); \
            if (type == R_X86_64_RELATIVE) { \
                uint64_t val = base_addr + r.addend; \
                write_vmm(ctx, target, &val, 8); \
            } else if ((type == R_X86_64_JUMP_SLOT || \
                        type == R_X86_64_GLOB_DAT || \
                        type == R_X86_64_64) && symtab && strtab) { \
                elf64_sym_t s; \
                read_vmm(ctx, &s, (uint64_t)symtab + (sidx * sizeof(elf64_sym_t)), sizeof(elf64_sym_t)); \
                char sname[128]; \
                read_vmm_str(ctx, (uint64_t)strtab + s.name, sname, sizeof(sname)); \
                uint64_t val = (s.shndx != 0) ? (base_addr + s.value) : resolve_symbol(sname, ctx); \
                if (!val) { \
                    printf("ELF Loader: Undefined symbol '%s'.\n", sname); \
                    return -ENOENT; \
                } \
                if (type == R_X86_64_64) val += r.addend; \
                write_vmm(ctx, target, &val, 8); \
            } \
        }

    if (rela && relasz)
        PROC_RELA(rela, relasz / sizeof(elf64_rela_t));
    if (jmprel && pltrelsz)
        PROC_RELA(jmprel, pltrelsz / sizeof(elf64_rela_t));

    #undef PROC_RELA
    return 0;
}

static void parse_dynamic(vmm_context_t *ctx, elf64_dyn_t *dynamic, uint64_t base_addr,
                           char **strtab, elf64_sym_t **symtab,
                           elf64_rela_t **rela, uint64_t *relasz,
                           elf64_rela_t **jmprel, uint64_t *pltrelsz) {
    for (int i = 0;; i++) {
        elf64_dyn_t d;
        read_vmm(ctx, &d, (uint64_t)dynamic + (i * sizeof(elf64_dyn_t)), sizeof(elf64_dyn_t));
        if (d.tag == DT_NULL) break;
        switch (d.tag) {
            case DT_STRTAB: *strtab = (char *)(base_addr + d.un.ptr); break;
            case DT_SYMTAB: *symtab = (elf64_sym_t *)(base_addr + d.un.ptr); break;
            case DT_RELA: *rela = (elf64_rela_t *)(base_addr + d.un.ptr); break;
            case DT_RELASZ: *relasz = d.un.val; break;
            case DT_JMPREL: *jmprel = (elf64_rela_t *)(base_addr + d.un.ptr); break;
            case DT_PLTRELSZ: *pltrelsz = d.un.val; break;
        }
    }
}

static const char *lib_search_paths[] = {
    "/usr/lib/",
    "/lib/",
    "",  // current directory
    NULL
};

static int load_shared_library(const char *soname, vmm_context_t *ctx) {
    uint64_t irq;

    spin_lock_irqsave(&elf_lock, &irq);
    for (loaded_so_t *so = loaded_libraries; so; so = so->next) {
        if (strcmp(so->name, soname) == 0) {
            spin_unlock_irqrestore(&elf_lock, irq);
            return 0;
        }
    }
    spin_unlock_irqrestore(&elf_lock, irq);

    const char *name = strip_relative(soname);

    // If soname doesn't start with / or ., search standard library paths
    rootfs_file_t file = { .data = NULL, .size = 0, .mode = 0 };
    if (soname[0] != '/' && soname[0] != '.') {
        for (int i = 0; lib_search_paths[i] != NULL; i++) {
            char full_path[256];
            strncpy(full_path, lib_search_paths[i], sizeof(full_path) - 1);
            full_path[sizeof(full_path) - 1] = '\0';
            strncat(full_path, name, sizeof(full_path) - strlen(full_path) - 1);
            file = read_rootfs(full_path);
            if (file.data) break;
        }
    } else {
        file = read_rootfs(soname);
    }

    if (!file.data) {
        printf("ELF Loader: Shared library '%s' not found.\n", name);
        return -ENOENT;
    }

    uint8_t *data = (uint8_t *)file.data;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (ehdr->magic != ELF_MAGIC || ehdr->type != ET_DYN) {
        printf("ELF Loader: '%s' is not a valid shared library.\n", name);
        return -ENOEXEC;
    }

    spin_lock_irqsave(&elf_lock, &irq);
    uint64_t base_addr = next_so_base;
    next_so_base += 0x10000000;
    spin_unlock_irqrestore(&elf_lock, irq);

    loaded_so_t *so = malloc(sizeof(loaded_so_t));
    if (!so) return -ENOMEM;
    memset(so, 0, sizeof(loaded_so_t));
    strncpy(so->name, soname, sizeof(so->name) - 1);
    so->base = base_addr;

    elf64_phdr_t *phdrs = (elf64_phdr_t *)(data + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdrs[i].type == PT_LOAD) {
            uint64_t start = base_addr + phdrs[i].vaddr;
            uint64_t end = start + phdrs[i].memsz;
            for (uint64_t a = start & ~0xFFFULL; a < ((end + 4095) & ~0xFFFULL); a += 4096) {
                if (get_vmm_phys(ctx, a) == 0) {
                    map_vmm(ctx, a, (uint64_t)pmalloc(), VMM_USER | VMM_WRITABLE);
                    memset_vmm(ctx, a, 0, 4096);
                }
            }
            if (phdrs[i].filesz > 0)
                write_vmm(ctx, start, data + phdrs[i].offset, phdrs[i].filesz);
        } else if (phdrs[i].type == PT_DYNAMIC) {
            so->dynamic = (elf64_dyn_t *)(base_addr + phdrs[i].vaddr);
        }
    }

    spin_lock_irqsave(&elf_lock, &irq);
    so->next = loaded_libraries;
    loaded_libraries = so;
    spin_unlock_irqrestore(&elf_lock, irq);

    if (!so->dynamic) return 0;

    elf64_rela_t *rela = NULL; uint64_t relasz = 0;
    elf64_rela_t *jmprel = NULL; uint64_t pltrelsz = 0;
    parse_dynamic(ctx, so->dynamic, base_addr,
                  &so->strtab, &so->symtab,
                  &rela, &relasz, &jmprel, &pltrelsz);

    if (so->strtab) {
        for (int i = 0;; i++) {
            elf64_dyn_t d;
            read_vmm(ctx, &d, (uint64_t)so->dynamic + (i * sizeof(elf64_dyn_t)), sizeof(elf64_dyn_t));
            if (d.tag == DT_NULL) break;
            if (d.tag == DT_NEEDED) {
                char needed[128];
                read_vmm_str(ctx, (uint64_t)so->strtab + d.un.val, needed, sizeof(needed));
                int ret = load_shared_library(needed, ctx);
                if (ret < 0) return ret;
            }
        }
    }

    return process_relocations(ctx, base_addr,
                               rela, relasz, jmprel, pltrelsz,
                               so->symtab, so->strtab);
}

static uint64_t setup_stack(vmm_context_t *ctx, uint64_t v_rsp,
                             char **argv, char **envp) {
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

            uint64_t page_start = seg_start & ~0xFFFULL;
            uint64_t page_end   = (seg_end + 0xFFFULL) & ~0xFFFULL;

            for (uint64_t a = page_start; a < page_end; a += 0x1000) {
                if (get_vmm_phys(ctx, a) == 0) {
                    void *page = pmalloc();
                    map_vmm(ctx, a, (uint64_t)page, VMM_USER | VMM_WRITABLE);
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

        } else if (ph->type == PT_DYNAMIC && dynamic_out) {
            *dynamic_out = (elf64_dyn_t *)(base_addr + ph->vaddr);
        }
    }

    return 0;
}

static int load_dependencies_and_relocate(vmm_context_t *ctx, elf64_dyn_t *dynamic,
                                           uint64_t base_addr) {
    if (!dynamic) return 0;

    char *strtab = NULL;
    elf64_sym_t *symtab = NULL;
    elf64_rela_t *rela = NULL; uint64_t relasz = 0;
    elf64_rela_t *jmprel = NULL; uint64_t pltrelsz = 0;

    parse_dynamic(ctx, dynamic, base_addr,
                  &strtab, &symtab, &rela, &relasz, &jmprel, &pltrelsz);

    if (strtab) {
        for (int i = 0;; i++) {
            elf64_dyn_t d;
            read_vmm(ctx, &d, (uint64_t)dynamic + (i * sizeof(elf64_dyn_t)), sizeof(elf64_dyn_t));
            if (d.tag == DT_NULL) break;
            if (d.tag == DT_NEEDED) {
                char so_name[128];
                read_vmm_str(ctx, (uint64_t)strtab + d.un.val, so_name, sizeof(so_name));
                int ret = load_shared_library(so_name, ctx);
                if (ret < 0) return ret;
            }
        }
    }

    return process_relocations(ctx, base_addr,
                               rela, relasz, jmprel, pltrelsz,
                               symtab, strtab);
}

pid_t execute_elf(const char *path, char **argv, char **envp) {
    uint64_t irq;
    spin_lock_irqsave(&elf_lock, &irq);
    loaded_libraries = NULL;
    next_so_base = 0x4000000000ULL;
    spin_unlock_irqrestore(&elf_lock, irq);

    if (devfs_device_exists(path))
        return -EACCES;

    rootfs_file_t file = read_rootfs(path);
    if (!file.data) {
        printf("ELF Loader: '%s' not found.\n", path);
        return -ENOENT;
    }

    uint8_t *data = (uint8_t *)file.data;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (ehdr->magic != ELF_MAGIC || ehdr->class != ELF_CLASS64 || ehdr->machine != EM_X86_64) {
        printf("ELF Loader: '%s' is not a valid ELF64 executable.\n", path);
        return -ENOEXEC;
    }

    vmm_context_t *ctx = create_vmm_context();
    if (!ctx) return -ENOMEM;

    uint64_t base_addr = (ehdr->type == ET_DYN) ? 0x100000000ULL : 0;
    elf64_dyn_t *dynamic = NULL;

    load_elf_segments(ctx, data, ehdr, base_addr, &dynamic);

    int ret = load_dependencies_and_relocate(ctx, dynamic, base_addr);
    if (ret < 0) return ret;

    void *stack = vmalloc_user_ex(ctx, 16384);
    if (!stack) return -ENOMEM;

    char *empty_envp[] = { NULL };
    char **actual_envp = envp ? envp : empty_envp;

    uint64_t v_rsp = setup_stack(ctx, (uint64_t)stack + 16384 - 8, argv, actual_envp);
    uint64_t entry = ehdr->entry + base_addr;

    // Find highest loaded address
    uint64_t heap_start = 0;
    elf64_phdr_t *phdrs = (elf64_phdr_t *)(data + ehdr->phoff);
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
    } else {
        printf("ELF Loader: Failed to create task for '%s'.\n", path);
    }

    return pid;
}

int execve_elf(const char *path, char **argv, char **envp, void* raw_frame) {
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;

    uint64_t irq;
    spin_lock_irqsave(&elf_lock, &irq);
    loaded_libraries = NULL;
    next_so_base = 0x4000000000ULL;
    spin_unlock_irqrestore(&elf_lock, irq);

    if (devfs_device_exists(path))
        return -EACCES;

    rootfs_file_t file = read_rootfs(path);
    if (!file.data) {
        printf("ELF Loader: '%s' not found.\n", path);
        return -ENOENT;
    }

    uint8_t *data = (uint8_t *)file.data;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (ehdr->magic != ELF_MAGIC || ehdr->class != ELF_CLASS64 || ehdr->machine != EM_X86_64)
        return -ENOEXEC;

    vmm_context_t *ctx = create_vmm_context();
    if (!ctx) return -ENOMEM;

    uint64_t base_addr = (ehdr->type == ET_DYN) ? 0x100000000ULL : 0;
    elf64_dyn_t *dynamic = NULL;

    load_elf_segments(ctx, data, ehdr, base_addr, &dynamic);

    int ret = load_dependencies_and_relocate(ctx, dynamic, base_addr);
    if (ret < 0) return ret;

    void *stack = vmalloc_user_ex(ctx, 16384);
    if (!stack) return -ENOMEM;

    char *empty_envp[] = { NULL };
    char **actual_envp = (envp) ? envp : empty_envp;

    uint64_t v_rsp = setup_stack(ctx, (uint64_t)stack + 16384 - 8, argv, actual_envp);
    uint64_t entry = ehdr->entry + base_addr;

    // Find highest loaded address
    uint64_t heap_start = 0;
    elf64_phdr_t *phdrs = (elf64_phdr_t *)(data + ehdr->phoff);
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