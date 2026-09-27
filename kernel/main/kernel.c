// Look at this #include mess...
#include <freestanding/stdbool.h>
#include <sys/utsname.h>
#include <binfmt/elf.h>
// Please, let this stop...
#include <crypto/rng.h>
#include <drivers/acpi/acpi.h>
// Almost...there...
#include <drivers/acpi/power_button.h>
#include <drivers/clock/rtc.h>
#include <drivers/devices/devices.h>
#include <drivers/fb/fb.h>
#include <drivers/fb/misc/fonts.h>
#include <drivers/net/dhcp.h>
#include <drivers/pci/pci.h>
#include <drivers/ps2/ps2_keyboard.h>
#include <drivers/serial/serial.h>
#include <drivers/snd/snd.h>
#include <drivers/timer/hpet.h>
#include <drivers/timer/pit.h>
#include <drivers/tty/pty.h>
#include <drivers/tty/tty.h>
#include <fs/initrd.h>
#include <fs/tmpfs.h>
// Are we there yet?
#include <main/boot_args.h>
#include <main/cpu_info.h>
#include <main/gdt.h>
#include <main/halt.h>
#include <main/idt.h>
#include <main/kernel.h>
#include <main/limine_req.h>
#include <main/panic.h>
#include <main/pic.h>
#include <main/smp.h>
#include <main/sse.h>
#include <main/stack_protector.h>
#include <main/terminal.h>
#include <main/apic/apic.h>
#include <main/apic/madt.h>
#include <mm/kstack.h>
#include <mm/mm.h>
#include <mm/oom.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sched/workqueue.h>
#include <syscalls/syscalls.h>
// Lets never do that again.
#include <util/string.h>

__attribute__((noreturn)) void kmain(void) {
    cli();
    // Check if we have a framebuffer given by Limine
    if (fb_req.response && fb_req.response->framebuffer_count >= 1) current_fb_driver = FB_LIMINE;
    clear_screen();
    init_serial_ports();
    init_default_font();
    show_cursor(true); // Show cursor as soon as possible
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) panic("base revision not supported");
    init_sse();
    init_pmm();
    init_vmm();
    init_mm();
    init_terminal_backbuffer();
    init_gdt();
    init_idt();
    remap_pic();
    init_acpi();
    parse_madt();
    detect_apic();
    init_apic();
    init_hpet();
    init_rtc();
    init_pit(250);
    init_rng();
    init_stack_protector();
    init_pci();
    init_pci_drivers();
    cache_cpu_info();
    cache_utsname();
    init_tty();
    init_pty();
    init_ps2_kbd();
    init_devices();
    init_snd();
    init_tmpfs();
    init_sched();
    init_syscalls();
    configure_dhcp();

    if (current_apic_mode != APIC_NONE) {
        init_apic_timer(250);
        init_smp();
    }

    init_initrd();

    if (!current_task_ptr || !current_task_ptr->kstack) panic("bsp kernel stack is unavailable");
    set_tss_kstack(kstack_top(current_task_ptr->kstack));

    sti();

    // Execute init process
    const char *init_path = "/init";
    char *init_argv[] = { (char*)init_path, NULL };
    int init = execute_elf(init_path, init_argv, NULL);
    if (init < 0) panic("init process didn't run due to an error");
    start_kernel_workqueue();

    idle();
    __builtin_unreachable();
}
