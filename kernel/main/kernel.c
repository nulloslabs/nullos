// Look at this #include mess...
#include <stdbool.h>
#include <main/panic.h>
#include <main/gdt.h>
#include <main/idt.h>
#include <main/sched.h>
#include <main/limine_req.h>
#include <main/acpi.h>
// Are we there yet?
#include <main/boot_args.h>
#include <main/sse.h>
#include <main/machine_info.h>
#include <main/halt.h>
#include <main/mp.h>
#include <main/elf.h>
#include <main/string.h>
#include <main/madt.h>
#include <main/utsname.h>
// Please, let this stop...
#include <main/rng.h>
#include <main/stack_protector.h>
#include <io/terminal.h>
#include <io/fb.h>
#include <io/initrd.h>
#include <io/devices.h>
#include <io/dhcp.h>
#include <io/hpet.h>
#include <io/rtc.h>
// Almost...there...
#include <io/fonts.h>
#include <io/pci.h>
#include <io/pic.h>
#include <io/pit.h>
#include <io/apic.h>
#include <io/ttys.h>
#include <io/ptys.h>
#include <io/serial.h>
#include <io/tmpfs.h>
#include <io/ps2_keyboard.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/oom.h>
#include <syscalls/syscalls.h>
// Lets never do that again.

__attribute__((noreturn)) void kmain(void) {
    cli();
    clrscr();
    // Check if we have a framebuffer given by Limine
    if (fb_req.response && fb_req.response->framebuffer_count >= 1) current_fb_driver = FB_LIMINE;
    init_serial_ports();
    init_default_font();
    show_cursor(true); // Show cursor as soon as possible
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) panic("base revision not supported");
    init_sse();
    init_gdt();
    init_idt();
    remap_pic();
    init_pmm();
    init_vmm();
    init_mm();
    init_terminal_backbuffer();
    init_initrd();
    init_tmpfs();
    init_ttys();
    init_ptys();
    init_acpi_tables();
    parse_madt();
    detect_apic();
    init_apic();
    init_hpet();
    init_rtc();
    init_pit();
    init_rng();
    init_stack_protector();
    init_sched();
    init_pci();
    init_acpi_namespace();
    flush_ps2_keyboard_controller();
    cache_machine_info();
    cache_utsname();
    init_pci_drivers();
    configure_dhcp();
    init_devices();
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
    if (init < 0) panic("init process didn't run due to an error");

    idle();
}
