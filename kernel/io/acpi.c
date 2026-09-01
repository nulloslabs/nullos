#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <main/log.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <main/idt.h>
#include <main/panic.h>
#include <io/acpi.h>
#include <io/apic.h>
#include <io/ec.h>
#include <io/time.h>
#include <io/power_button.h>
#include <io/io.h>
#include <io/ioapic.h>
#include <io/pci.h>
#include <io/pic.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/kernel_api.h>
#include <uacpi/status.h>

static uacpi_irq_t *acpi_irq;
static spinlock_t pci_config_lock = SPINLOCK_INIT;
static spinlock_t acpi_log_lock = SPINLOCK_INIT;
static uint64_t early_table_buffer[512];
static char acpi_log_buffer[1024];
static uacpi_bool early_tables_ready;

static uacpi_u64 monotonic_ms(void) {
    return get_monotonic_time_us() / 1000;
}

static uint32_t pci_read_dword(uacpi_pci_handle_t *device, uacpi_size offset) {
    return read_pci(device->address.bus, device->address.device, device->address.function, (uint8_t)offset);
}

static void pci_write_dword(uacpi_pci_handle_t *device, uacpi_size offset, uint32_t value) {
    write_pci(device->address.bus, device->address.device, device->address.function, (uint8_t)offset, value);
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    if (!out_rsdp_address || !rsdp_req.response || !rsdp_req.response->address) return UACPI_STATUS_NOT_FOUND;
    uintptr_t rsdp = (uintptr_t)rsdp_req.response->address;
    *out_rsdp_address = rsdp >= hhdm_offset ? rsdp - hhdm_offset : rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (!len || len > SIZE_MAX - (addr & (PAGE_SIZE - 1))) return UACPI_MAP_FAILED;
    uacpi_size span = (addr & (PAGE_SIZE - 1)) + len;
    uacpi_size pages = (span + PAGE_SIZE - 1) / PAGE_SIZE;
    void *mapping = vmap_mmio(addr, pages);
    return mapping ? mapping : UACPI_MAP_FAILED;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    if (!addr || addr == UACPI_MAP_FAILED || !len) return;

    uacpi_size offset = (uintptr_t)addr & (PAGE_SIZE - 1);
    if (len > SIZE_MAX - offset) return;

    uacpi_size span = offset + len;
    uacpi_size pages = (span + PAGE_SIZE - 1) / PAGE_SIZE;

    vunmap_mmio(addr, pages);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *message) {
    (void)level;
    // I like lowercase logs :P
    uint64_t flags;
    spin_lock_irqsave(&acpi_log_lock, &flags);
    size_t length = 0;
    while (message[length] && length + 1 < sizeof(acpi_log_buffer)) {
        acpi_log_buffer[length] = tolower((unsigned char)message[length]);
        length++;
    }
    acpi_log_buffer[length] = '\0';
    log("acpi: %s", acpi_log_buffer);
    spin_unlock_irqrestore(&acpi_log_lock, flags);
}

void *uacpi_kernel_alloc(uacpi_size size) {
    return malloc(size);
}

void uacpi_kernel_free(void *mem) {
    free(mem);
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    if (!out_handle || address.segment != 0) return UACPI_STATUS_NOT_FOUND;
    uacpi_pci_handle_t *device = malloc(sizeof(*device));
    if (!device) return UACPI_STATUS_OUT_OF_MEMORY;
    device->address = address;
    *out_handle = device;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    free(handle);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *value) {
    if (!handle || !value || offset >= 256) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    uint32_t data = pci_read_dword(handle, offset);
    spin_unlock_irqrestore(&pci_config_lock, flags);
    *value = (data >> ((offset & 3) * 8)) & 0xFF;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *value) {
    if (!handle || !value || offset >= 255 || (offset & 1)) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    uint32_t data = pci_read_dword(handle, offset);
    spin_unlock_irqrestore(&pci_config_lock, flags);
    *value = (data >> ((offset & 2) * 8)) & 0xFFFF;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *value) {
    if (!handle || !value || offset >= 253 || (offset & 3)) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    *value = pci_read_dword(handle, offset);
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
    if (!handle || offset >= 256) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    uint32_t data = pci_read_dword(handle, offset);
    uint32_t shift = (offset & 3) * 8;
    pci_write_dword(handle, offset, (data & ~(0xFFu << shift)) | ((uint32_t)value << shift));
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
    if (!handle || offset >= 255 || (offset & 1)) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    uint32_t data = pci_read_dword(handle, offset);
    uint32_t shift = (offset & 2) * 8;
    pci_write_dword(handle, offset, (data & ~(0xFFFFu << shift)) | ((uint32_t)value << shift));
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
    if (!handle || offset >= 253 || (offset & 3)) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    pci_write_dword(handle, offset, value);
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    if (!out_handle || base > 0xFFFF || len > 0x10000 || base + len > 0x10000) return UACPI_STATUS_INVALID_ARGUMENT;
    *out_handle = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    (void)handle;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value) {
    if (!out_value || (uintptr_t)handle + offset > 0xFFFF) return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inb((uint16_t)((uintptr_t)handle + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value) {
    if (!out_value || (uintptr_t)handle + offset > 0xFFFE) return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inw((uint16_t)((uintptr_t)handle + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value) {
    if (!out_value || (uintptr_t)handle + offset > 0xFFFC) return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inl((uint16_t)((uintptr_t)handle + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value) {
    if ((uintptr_t)handle + offset > 0xFFFF) return UACPI_STATUS_INVALID_ARGUMENT;
    outb((uint16_t)((uintptr_t)handle + offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value) {
    if ((uintptr_t)handle + offset > 0xFFFE) return UACPI_STATUS_INVALID_ARGUMENT;
    outw((uint16_t)((uintptr_t)handle + offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value) {
    if ((uintptr_t)handle + offset > 0xFFFC) return UACPI_STATUS_INVALID_ARGUMENT;
    outl((uint16_t)((uintptr_t)handle + offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    return get_monotonic_time_us() * 1000;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    sleep_us(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    sleep(msec);
}

uacpi_handle uacpi_kernel_create_mutex(void) {
    uacpi_lock_t *mutex = malloc(sizeof(*mutex));
    if (mutex) mutex->lock = SPINLOCK_INIT;
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    free(handle);
}

uacpi_handle uacpi_kernel_create_event(void) {
    uacpi_event_t *event = malloc(sizeof(*event));
    if (event) {
        event->lock = SPINLOCK_INIT;
        event->counter = 0;
    }
    return event;
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    free(handle);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id)(uintptr_t)(current_task + 1);
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uacpi_interrupt_state state;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(state) :: "memory");
    return state;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    __asm__ volatile ("push %0; popfq" :: "r"(state) : "memory");
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    if (!handle) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_lock_t *mutex = handle;
    uacpi_u64 start = monotonic_ms();
    do {
        if (__sync_bool_compare_and_swap(&mutex->lock, 0, 1)) return UACPI_STATUS_OK;
        __asm__ volatile ("pause");
    } while (timeout == 0xFFFF || monotonic_ms() - start < timeout);
    return UACPI_STATUS_TIMEOUT;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    if (handle) spin_unlock(&((uacpi_lock_t *)handle)->lock);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    if (!handle) return UACPI_FALSE;
    uacpi_event_t *event = handle;
    uacpi_u64 start = monotonic_ms();
    do {
        uint64_t flags;
        spin_lock_irqsave(&event->lock, &flags);
        if (event->counter) {
            event->counter--;
            spin_unlock_irqrestore(&event->lock, flags);
            return UACPI_TRUE;
        }
        spin_unlock_irqrestore(&event->lock, flags);
        __asm__ volatile ("pause");
    } while (timeout == 0xFFFF || monotonic_ms() - start < timeout);
    return UACPI_FALSE;
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    if (!handle) return;
    uacpi_event_t *event = handle;
    uint64_t flags;
    spin_lock_irqsave(&event->lock, &flags);
    if (event->counter != UINT32_MAX) event->counter++;
    spin_unlock_irqrestore(&event->lock, flags);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    if (!handle) return;
    uacpi_event_t *event = handle;
    uint64_t flags;
    spin_lock_irqsave(&event->lock, &flags);
    event->counter = 0;
    spin_unlock_irqrestore(&event->lock, flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    if (!request) return UACPI_STATUS_INVALID_ARGUMENT;
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT) return UACPI_STATUS_OK;
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL) panic("uACPI fatal request: type=%u code=%u arg=%llx", request->fatal.type, request->fatal.code, request->fatal.arg);
    return UACPI_STATUS_UNIMPLEMENTED;
}

void uacpi_irq_dispatch(void) {
    if (acpi_irq && acpi_irq->handler) acpi_irq->handler(acpi_irq->context);
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx, uacpi_handle *out_irq_handle) {
    if (!handler || !out_irq_handle || irq >= 16 || acpi_irq) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_irq_t *record = malloc(sizeof(*record));
    if (!record) return UACPI_STATUS_OUT_OF_MEMORY;
    record->irq = irq;
    record->handler = handler;
    record->context = ctx;
    acpi_irq = record;
    idt_set_descriptor((uint8_t)(32 + irq), acpi_isr, 0x8E);
    if (current_apic_mode == APIC_NONE) unmask_pic_irq((uint8_t)irq);
    else if (ioapic_base) route_ioapic_irq((uint8_t)irq, (uint8_t)(32 + irq), 0, IOAPIC_ACTIVE_LOW | IOAPIC_TRIGGER_LEVEL);
    *out_irq_handle = record;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    uacpi_irq_t *record = irq_handle;
    if (!record || record != acpi_irq || record->handler != handler) return UACPI_STATUS_INVALID_ARGUMENT;
    if (current_apic_mode == APIC_NONE) mask_pic_irq((uint8_t)record->irq);
    else if (ioapic_base) route_ioapic_irq((uint8_t)record->irq, (uint8_t)(32 + record->irq), 0, IOAPIC_INT_MASKED);
    acpi_irq = NULL;
    free(record);
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    uacpi_lock_t *lock = malloc(sizeof(*lock));
    if (lock) lock->lock = SPINLOCK_INIT;
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    free(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    uint64_t flags;
    spin_lock_irqsave(&((uacpi_lock_t *)handle)->lock, &flags);
    return flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    spin_unlock_irqrestore(&((uacpi_lock_t *)handle)->lock, flags);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx) {
    (void)type;
    if (!handler) return UACPI_STATUS_INVALID_ARGUMENT;
    handler(ctx);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK;
}

void init_acpi_tables(void) {
    uacpi_status status = uacpi_setup_early_table_access(early_table_buffer, sizeof(early_table_buffer));
    if (uacpi_unlikely_error(status)) {
        log("uacpi: early table initialization failed: %s\n", uacpi_status_to_string(status));
        return;
    }
    early_tables_ready = UACPI_TRUE;
    log("acpi: initialized acpi tables\n");
}

void init_acpi_namespace(void) {
    if (!early_tables_ready) init_acpi_tables();
    if (!early_tables_ready) return;
    uacpi_status status = uacpi_initialize(0);
    if (uacpi_unlikely_error(status)) {
        log("acpi: initialization failed: %s\n", uacpi_status_to_string(status));
        return;
    }
    status = uacpi_namespace_load();
    if (uacpi_unlikely_error(status)) {
        log("acpi: namespace load failed: %s\n", uacpi_status_to_string(status));
        return;
    }
    status = init_ec();
    if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) log("uacpi: ec initialization failed: %s\n", uacpi_status_to_string(status));
    uacpi_interrupt_model model = current_apic_mode == APIC_NONE ? UACPI_INTERRUPT_MODEL_PIC : UACPI_INTERRUPT_MODEL_IOAPIC;
    status = uacpi_set_interrupt_model(model);
    if (uacpi_unlikely_error(status)) log("uacpi: interrupt model selection failed: %s\n", uacpi_status_to_string(status));
    status = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(status)) {
        log("acpi: namespace initialization failed: %s\n", uacpi_status_to_string(status));
        return;
    }
    init_power_button();
    log("acpi: initialized acpi namespace\n");
}
