#include <drivers/acpi/power.h>
#include <main/halt.h>
#include <main/io.h>
#include <time/time.h>
#include <uacpi/sleep.h>

static volatile int started_power_transition = 0;

static void begin_power_transition(void) {
    cli();
    if (!__sync_lock_test_and_set(&started_power_transition, 1)) return;
    idle();
}

void reboot(void) {
    begin_power_transition();
    uacpi_reboot();
    if (!__sync_lock_test_and_set(&system_halted, 1)) halt_other_cpus();

    for (uint32_t attempt = 0; attempt < 100000; attempt++) {
        uint8_t status = inb(0x64);
        if (status == 0xFF) break;
        if (!(status & 0x02)) {
            outb(0xFE, 0x64);
            sleep(20);
            break;
        }
        __asm__ volatile ("pause");
    }

    outb(0x02, 0xCF9);
    outb(0x06, 0xCF9);
    halt();
}

void poweroff(void) {
    begin_power_transition();
    // Try emulator ports first and then actual uACPI poweroff
    outw(0x2000, 0x604);
    outw(0x2000, 0xB004);
    uacpi_enter_sleep_state_simple(UACPI_SLEEP_STATE_S5);
    halt();
}
