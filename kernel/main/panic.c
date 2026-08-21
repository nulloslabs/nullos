#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <signal.h>
#include <main/assert.h>
#include <main/log.h>
#include <main/elf.h>
#include <main/limine_req.h>
#include <main/panic.h>
#include <main/halt.h>
#include <main/sched.h>
#include <io/terminal.h>
#include <mm/pf.h>
#include <mm/kstack.h>
#include <syscalls/syscalls.h>
#include <syscalls/syscall_impls.h>

static bool is_elf_range_valid(uint64_t offset, uint64_t length, uint64_t file_size) {
    return offset <= file_size && length <= file_size - offset;
}

static bool find_kernel_symbol_table(kernel_symbol_table_t *table_out) {
    if (!cmdline_req.response || !cmdline_req.response->executable_file) return false;

    const struct limine_file *file = cmdline_req.response->executable_file;
    const uint8_t *image = (const uint8_t *)file->address;
    uint64_t file_size = file->size;
    if (!image || file_size < sizeof(elf64_ehdr_t)) return false;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)image;
    if (ehdr->magic != ELF_MAGIC || ehdr->class != ELF_CLASS64 ||
        ehdr->shentsize < sizeof(elf64_shdr_t) || ehdr->shnum == 0 ||
        !is_elf_range_valid(ehdr->shoff, (uint64_t)ehdr->shentsize * ehdr->shnum, file_size)) {
        return false;
    }

    uint64_t load_bias = 0;
    if (ehdr->phentsize >= sizeof(elf64_phdr_t) && ehdr->phnum > 0 &&
        is_elf_range_valid(ehdr->phoff, (uint64_t)ehdr->phentsize * ehdr->phnum, file_size) &&
        eaddr_req.response) {
        uint64_t linked_base = (uint64_t)-1;
        for (uint16_t i = 0; i < ehdr->phnum; i++) {
            const elf64_phdr_t *phdr = (const elf64_phdr_t *)(image + ehdr->phoff +
                                                              (uint64_t)i * ehdr->phentsize);
            if (phdr->type == PT_LOAD && phdr->vaddr < linked_base) linked_base = phdr->vaddr;
        }
        if (linked_base != (uint64_t)-1) load_bias = eaddr_req.response->virtual_base - linked_base;
    }

    // Prefer the full static symbol table, then fall back to the dynamic one.
    for (uint32_t wanted_type = SHT_SYMTAB; ; wanted_type = SHT_DYNSYM) {
        for (uint16_t section_idx = 0; section_idx < ehdr->shnum; section_idx++) {
            const elf64_shdr_t *symtab = (const elf64_shdr_t *)(image + ehdr->shoff +
                                                                (uint64_t)section_idx * ehdr->shentsize);
            if (symtab->type != wanted_type || symtab->entsize < sizeof(elf64_sym_t) ||
                symtab->link >= ehdr->shnum || symtab->size < symtab->entsize ||
                !is_elf_range_valid(symtab->offset, symtab->size, file_size)) continue;

            const elf64_shdr_t *strtab = (const elf64_shdr_t *)(image + ehdr->shoff +
                                                                (uint64_t)symtab->link * ehdr->shentsize);
            if (strtab->type != SHT_STRTAB ||
                !is_elf_range_valid(strtab->offset, strtab->size, file_size)) continue;

            if (table_out) {
                table_out->image = image;
                table_out->symtab = symtab;
                table_out->strtab = strtab;
                table_out->load_bias = load_bias;
            }
            return true;
        }
        if (wanted_type == SHT_DYNSYM) break;
    }

    return false;
}

bool are_kernel_symbols_available(void) {
    return find_kernel_symbol_table(NULL);
}

static bool kernel_symbol_for_rip(uint64_t rip, const char **name_out, uint64_t *offset_out) {
    if (!name_out || !offset_out) return false;

    kernel_symbol_table_t table;
    if (!find_kernel_symbol_table(&table)) return false;

    const char *best_name = NULL;
    uint64_t best_start = 0;
    uint64_t symbol_count = table.symtab->size / table.symtab->entsize;

    for (uint64_t i = 0; i < symbol_count; i++) {
        const elf64_sym_t *symbol = (const elf64_sym_t *)(table.image + table.symtab->offset +
                                                          i * table.symtab->entsize);
        if (ELF64_ST_TYPE(symbol->info) != STT_FUNC || symbol->shndx == SHN_UNDEF ||
            symbol->name == 0 || symbol->name >= table.strtab->size) continue;

        uint64_t start = symbol->value + table.load_bias;
        if (start > rip || (symbol->size != 0 && rip - start >= symbol->size) ||
            (best_name && start <= best_start)) continue;

        const char *name = (const char *)(table.image + table.strtab->offset + symbol->name);
        uint64_t remaining = table.strtab->size - symbol->name;
        bool terminated = false;
        for (uint64_t j = 0; j < remaining; j++) {
            if (name[j] == '\0') {
                terminated = true;
                break;
            }
        }
        if (!terminated || name[0] == '\0') continue;

        best_name = name;
        best_start = start;
    }

    if (!best_name) return false;
    *name_out = best_name;
    *offset_out = rip - best_start;
    return true;
}

static int exception_to_signal(int vector) {
    switch (vector) {
        case 0:  return SIGFPE;   // Division error
        case 4:  return SIGFPE;   // Overflow
        case 5:  return SIGFPE;   // Bounds
        case 6:  return SIGILL;   // Invalid opcode
        case 7:  return SIGFPE;   // Device not available (x87)
        case 8:  return SIGSEGV;  // Double fault
        case 10: return SIGSEGV;  // Invalid TSS
        case 11: return SIGBUS;   // Segment not present
        case 12: return SIGSEGV;  // Stack segment fault
        case 13: return SIGSEGV;  // General protection fault
        case 14: return SIGSEGV;  // Page fault
        case 16: return SIGFPE;   // x87 FP exception
        case 17: return SIGBUS;   // Alignment check
        case 18: return SIGBUS;   // Machine check
        case 19: return SIGFPE;   // SIMD FP exception
        case 30: return SIGSEGV;  // Security exception
        default: return SIGSEGV;
    }
}

__attribute__((noreturn)) void dopanic(const char *func, const char *msg, ...) {
    cli();

    va_list args;
    va_start(args, msg);

    uint64_t rip = (uint64_t)__builtin_return_address(0) - 1;
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

    // I hate this.
    control_log_console(SYSLOG_ACTION_CONSOLE_LEVEL, 7);

    log("kernel panic: ");
    vlog(msg, args);
    log("\n");
    log("\nregisters:\n");
    const char *symbol_name;
    uint64_t symbol_offset;
    if (are_kernel_symbols_available() &&
        kernel_symbol_for_rip(rip, &symbol_name, &symbol_offset)) {
        log("  rip: %p (%s+0x%lx)\n", (void *)(uintptr_t)rip, symbol_name, symbol_offset);
    } else {
        // Fallback, no exact start address
        log("  rip: %p (%s+0x?)\n", (void *)(uintptr_t)rip, func);
    }
    log("  rsp: %p\n", (void *)(uintptr_t)rsp);

    va_end(args);
    halt();
    __builtin_unreachable();
}

void exception_panic(exception_frame_t *frame) {
    assert(frame != NULL);
    const char *reason = "";

    switch (frame->vector) {
        case 0: reason = "a division error occurred"; break;
        case 4: reason = "a signed arithmetic overflow occurred"; break;
        case 5: reason = "an instruction that exceeded the bound range occurred"; break;
        case 6: reason = "a invalid opcode instruction occurred"; break;
        case 7: reason = "an instruction tried to access a device that was not available"; break;
        case 8: reason = "a double fault occurred"; break;
        case 13: reason = "a general protection fault occurred"; break;
        case 14: reason = "a page fault occurred"; break;
        case 30: reason = "a security exception occurred"; break;
        case 512: reason = "a reserved exception was called"; break;
        default: reason = "an unknown exception occurred"; break;
    }
    if ((frame->vector == 8 || frame->vector == 14) && !(frame->cs & 3) && is_kstack_guard(frame->cr2)) reason = "a kernel stack overflow occurred";

    // Check if the fault came from user mode (Ring 3)
    if ((frame->cs & 3) != 0) {
        if (frame->vector == 14) {
            int page_status = handle_pf(frame->cr2, frame->error_code);
            if (page_status == 0) return;
        }

        int sig = exception_to_signal(frame->vector);
        current_task_ptr->pending_signals |= (1ULL << sig);

        syscall_frame_t signal_frame = {0};
        signal_frame.rax = frame->rax;
        signal_frame.rbx = frame->rbx;
        signal_frame.rcx = frame->rcx;
        signal_frame.rdx = frame->rdx;
        signal_frame.rsi = frame->rsi;
        signal_frame.rdi = frame->rdi;
        signal_frame.rsp = frame->rsp;
        signal_frame.rbp = frame->rbp;
        signal_frame.r8  = frame->r8;
        signal_frame.r9  = frame->r9;
        signal_frame.r10 = frame->r10;
        signal_frame.r11 = frame->r11;
        signal_frame.r12 = frame->r12;
        signal_frame.r13 = frame->r13;
        signal_frame.r14 = frame->r14;
        signal_frame.r15 = frame->r15;
        signal_frame.rip = frame->rip;
        signal_frame.rflags = frame->rflags;

        check_signals_from_user_exception(&signal_frame);

        frame->rax = signal_frame.rax;
        frame->rbx = signal_frame.rbx;
        frame->rcx = signal_frame.rcx;
        frame->rdx = signal_frame.rdx;
        frame->rsi = signal_frame.rsi;
        frame->rdi = signal_frame.rdi;
        frame->rsp = signal_frame.rsp;
        frame->rbp = signal_frame.rbp;
        frame->r8  = signal_frame.r8;
        frame->r9  = signal_frame.r9;
        frame->r10 = signal_frame.r10;
        frame->r11 = signal_frame.r11;
        frame->r12 = signal_frame.r12;
        frame->r13 = signal_frame.r13;
        frame->r14 = signal_frame.r14;
        frame->r15 = signal_frame.r15;
        frame->rip = signal_frame.rip;
        frame->rflags = signal_frame.rflags;
        return;
    }

    // Kernel fault, panic as before
    cli();
    control_log_console(SYSLOG_ACTION_CONSOLE_LEVEL, 7);
    log("kernel panic: %s\n", reason);
    log("\nregisters:\n");
    const char *symbol_name;
    uint64_t symbol_offset;
    if (are_kernel_symbols_available() &&
        kernel_symbol_for_rip(frame->rip, &symbol_name, &symbol_offset)) {
        log("  rip: %p (%s+0x%lx)\n", (void *)(uintptr_t)frame->rip, symbol_name, symbol_offset);
    } else {
        // Fallback, no debug info
        log("  rip: %p (?+0x?)\n", (void *)(uintptr_t)frame->rip);
    }
    log("  rsp: %p\n", (void *)(uintptr_t)frame->rsp);
    halt();
    __builtin_unreachable();
}
