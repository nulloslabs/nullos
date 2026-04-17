#include <freestanding/stddef.h>
#include <io/terminal.h>
#include <main/panic.h>
#include <main/rootfs.h>
#include <main/devfs.h>
#include <mm/mm.h>
#include <main/gdt.h>
#include <main/idt.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <io/hpet.h>
#include <main/scheduler.h>
#include <main/limine_req.h>
#include <main/acpi.h>
#include <io/font.h>
#include <main/boot_args.h>
#include <io/pci.h>
#include <main/sse.h>
#include <main/machine_info.h>
#include <main/halt.h>
#include <io/pic.h>
#include <io/pit.h>
#include <io/apic.h>
#include <main/mp.h>
#include <main/elf.h>
#include <main/string.h>
#include <syscalls/syscalls.h>
#include <main/madt.h>

void kmain(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    cli();
    clrscr();
    parse_boot_args();
    init_sse();
    init_gdt();
    init_idt();
    remap_pic();
    init_heap();
    init_rootfs();
    init_devfs();
    if (!((fb->width == 640 && fb->height == 480) || (fb->width == 800 && fb->height == 600))) panic("Unsupported resolution.");
    show_cursor(true);
    init_pmm();
    init_vmm();
    init_acpi();
    parse_madt();
    detect_apic();
    init_apic();
    init_hpet();
    init_pit(250);
    init_scheduler();
    init_pci();
    cache_machine_info();
    init_pci_drivers();
    init_syscalls();

    if (current_apic_mode != APIC_NONE) {
        init_mp();
        init_apic_timer(250);
    }

    void* kernel_stack = malloc(32768);
    set_tss_kernel_stack((void*)((uint64_t)kernel_stack + 32768));

    sti();

    const char *init_path = "/init";
    char *init_argv[] = { (char*)init_path, NULL };
    char *init_envp[] = { NULL };
    pid_t init = execute_elf(init_path, init_argv, init_envp);
    if (init < 0) panic("Init process didn't run due to a error.");

    idle();
}
