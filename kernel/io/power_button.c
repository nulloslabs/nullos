#include <signal.h>
#include <main/sched.h>
#include <main/signal.h>
#include <main/workqueue.h>
#include <main/log.h>
#include <io/power.h>
#include <io/power_button.h>
#include <uacpi/event.h>

static void process_power_button(void *context) {
    (void)context;
    int init_index = task_index_by_pid(1);
    if (init_index >= 0 && send_task_signal(init_index, SIGUSR2)) return;
    poweroff();
}

static uacpi_interrupt_ret handle_power_button(uacpi_handle context) {
    (void)context;
    if (!queue_kernel_work(process_power_button, NULL)) log("acpi: power button work queue is full\n");
    return UACPI_INTERRUPT_HANDLED;
}

void init_power_button(void) {
    uacpi_status status = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, handle_power_button, NULL);
    if (uacpi_unlikely_error(status)) {
        log("acpi: power button initialization failed: %s\n", uacpi_status_to_string(status));
        return;
    }

    log("acpi: initialized power button\n");
}
