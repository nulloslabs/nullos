#include <io/power.h>
#include <main/halt.h>
#include <io/io.h>
#include <io/time.h>
#include <uacpi/sleep.h>

static volatile int started_power_transition;

static void begin_power_transition(void) {
    cli();
    if (!__sync_lock_test_and_set(&started_power_transition, 1)) return;
    for (;;) __asm__ volatile("hlt" : : : "memory");
}

void poweroff(void) {
    begin_power_transition();
    uacpi_enter_sleep_state_simple(UACPI_SLEEP_STATE_S5);
    halt();
}

void reboot(void) {
    begin_power_transition();
    uacpi_reboot();
    if (!__sync_lock_test_and_set(&system_halted, 1)) halt_other_cpus();
    for (uint32_t attempt = 0; attempt < 100000; attempt++) {
        uint8_t status = inb(0x64);
        if (status == 0xFF) break;
        if (!(status & 0x02)) {
            outb(0x64, 0xFE);
            sleep(20);
            break;
        }
        __asm__ volatile("pause");
    }
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    halt();
}
