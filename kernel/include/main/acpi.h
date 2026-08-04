#pragma once

#include <main/spinlocks.h>
#include <uacpi/kernel_api.h>

typedef struct {
    uacpi_pci_address address;
} uacpi_pci_handle_t;

typedef struct {
    spinlock_t lock;
} uacpi_lock_t;

typedef struct {
    spinlock_t lock;
    uacpi_u32 counter;
} uacpi_event_t;

typedef struct {
    uacpi_u32 irq;
    uacpi_interrupt_handler handler;
    uacpi_handle context;
} uacpi_irq_t;

void init_acpi_tables(void);
void init_acpi_namespace(void);
