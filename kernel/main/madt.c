#include <freestanding/stddef.h>
#include <main/acpi.h>
#include <main/madt.h>
#include <io/terminal.h>

void* ioapic_phys_addr = NULL;

void parse_madt(void) {
    struct acpi_header* madt = find_acpi_table("APIC");
    if (!madt) return;

    struct madt_header* madt_hdr = (struct madt_header*)((uint8_t*)madt + sizeof(struct acpi_header));
    uint8_t* ptr = (uint8_t*)madt_hdr + sizeof(struct madt_header);
    uint8_t* end = (uint8_t*)madt + madt->length;

    while (ptr < end) {
        struct madt_record* rec = (struct madt_record*)ptr;
        if (rec->type == 1) { // IOAPIC
            uint32_t addr = *(uint32_t*)(ptr + 4);
            ioapic_phys_addr = (void*)(uint64_t)addr;
        }
        ptr += rec->length;
    }
    printf("MADT: Parsed MADT.\n");
}