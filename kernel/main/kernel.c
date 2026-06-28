// Look at this #include mess...
#include <io/terminal.h>
#include <io/framebuffer.h>
#include <main/panic.h>
#include <main/rootfs.h>
#include <io/devices.h>
#include <mm/mm.h>
// Are we there yet?
#include <main/gdt.h>
#include <main/idt.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <io/hpet.h>
#include <io/rtc.h>
#include <main/scheduler.h>
#include <main/limine_req.h>
#include <main/acpi.h>
// Please, let this stop...
#include <io/fonts.h>
#include <main/boot_args.h>
#include <io/pci.h>
#include <main/sse.h>
#include <main/machine_info.h>
#include <main/halt.h>
#include <io/pic.h>
#include <io/pit.h>
// Almost...there...
#include <io/apic.h>
#include <main/mp.h>
#include <main/elf.h>
#include <main/string.h>
#include <syscalls/syscalls.h>
#include <main/madt.h>
#include <main/utsname.h>
#include <io/ttys.h>
#include <io/ptys.h>
#include <main/rng.h>
// Lets never do that again.

void kmain(uint64_t load_offset) {
    cli();
    clrscr();
    init_default_font();
    parse_boot_args();
    init_sse();
    init_gdt();
    init_idt();
    remap_pic();
    init_mm();
    init_pmm();
    init_vmm();
    init_rootfs();
    init_devices();
    init_ttys();
    init_ptys();
    show_cursor(true);
    init_acpi();
    parse_madt();
    detect_apic();
    init_apic();
    init_hpet();
    init_rtc();
    init_pit(250);
    init_rng();
    init_scheduler();
    init_pci();
    cache_machine_info();
    cache_utsname();
    init_pci_drivers();
    init_syscalls();

    if (current_apic_mode != APIC_NONE) { init_mp(); init_apic_timer(250); }

    void* kernel_stack = malloc(32768);
    set_tss_kernel_stack((void*)((uint64_t)kernel_stack + 32768));

    sti();

    // Execute init process
    const char *init_path = "/init";
    char *init_argv[] = { (char*)init_path, NULL };
    char *init_envp[] = { NULL };
    int init = execute_elf(init_path, init_argv, init_envp);
    if (init < 0) panic("init process didn't run due to a error");

    idle();
}
