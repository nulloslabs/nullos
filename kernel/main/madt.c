#include <stdint.h>
#include <main/madt.h>
#include <io/terminal.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

void *ioapic_phys_addr;

void parse_madt(void) {
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) return;
    struct acpi_madt *madt = table.ptr;
    struct acpi_entry_hdr *entry = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->hdr.length;
    while ((uint8_t *)entry + sizeof(*entry) <= end && entry->length >= sizeof(*entry) && (uint8_t *)entry + entry->length <= end) {
        if (entry->type == ACPI_MADT_ENTRY_TYPE_IOAPIC && entry->length >= sizeof(struct acpi_madt_ioapic)) {
            struct acpi_madt_ioapic *ioapic = (struct acpi_madt_ioapic *)entry;
            ioapic_phys_addr = (void *)(uintptr_t)ioapic->address;
            break;
        }
        entry = (struct acpi_entry_hdr *)((uint8_t *)entry + entry->length);
    }
    uacpi_table_unref(&table);
    printf("madt: parsed madt\n");
}
