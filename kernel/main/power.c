#include <main/power.h>
#include <main/halt.h>
#include <io/io.h>
#include <uacpi/sleep.h>

void poweroff(void) {
    uacpi_enter_sleep_state_simple(UACPI_SLEEP_STATE_S5);
    halt();
}

void reboot(void) {
    cli();
    uacpi_reboot();
    while (inb(0x64) & 0x02) __asm__ volatile("pause");
    outb(0x64, 0xFE);
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    halt();
}
