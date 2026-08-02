#include <stddef.h>
#include <main/power.h>
#include <main/string.h>
#include <main/acpi.h>
#include <io/io.h>
#include <io/hpet.h>
#include <main/halt.h>
#include <main/spinlocks.h>
#include <io/terminal.h>
#include <mm/mm.h>

static uint16_t slp_typa = 0xFFFF;
static uint16_t slp_typb = 0xFFFF;

// ---- S5 sleep-type discovery ----

static uint32_t aml_read_int_s5(uint8_t** p) {
    uint8_t op = *(*p)++;
    switch (op) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0xFF: return 0xFFFFFFFF;
        case 0x0A: return *(*p)++;
        case 0x0B: { uint16_t r = *(uint16_t*)*p; *p += 2; return r; }
        case 0x0C: { uint32_t r = *(uint32_t*)*p; *p += 4; return r; }
        default:   return 0;
    }
}

static uint32_t aml_pkg_len_s5(uint8_t** p) {
    uint8_t lead = *(*p)++;
    uint32_t len = lead & 0x3F;
    uint32_t follow = lead >> 6;
    for (uint32_t i = 0; i < follow; i++) len |= (*(*p)++) << (4 + i * 8);
    return len;
}

static void scan_table_for_s5(struct acpi_header* h) {
    if (slp_typa != 0xFFFF) return;
    uint8_t* aml = (uint8_t*)h + sizeof(struct acpi_header);
    uint32_t len = h->length - sizeof(struct acpi_header);
    if (len < 8) return;
    for (uint32_t i = 0; i < len - 7; i++) {
        uint8_t* s5_ptr = NULL;
        if (aml[i] == 0x08 && memcmp(&aml[i+1], "_S5_", 4) == 0)
            s5_ptr = &aml[i+5];
        else if (aml[i] == 0x08 && aml[i+1] == 0x5C && memcmp(&aml[i+2], "_S5_", 4) == 0)
            s5_ptr = &aml[i+6];
        if (s5_ptr && *s5_ptr == 0x12) {
            s5_ptr++;
            aml_pkg_len_s5(&s5_ptr);
            uint8_t count = *s5_ptr++;
            slp_typa = aml_read_int_s5(&s5_ptr) & 0x7;
            slp_typb = (count > 1) ? (aml_read_int_s5(&s5_ptr) & 0x7) : slp_typa;
            return;
        }
    }
}

static void parse_s5(void) {
    if (!acpi_root || !fadt) return;
    if (slp_typa != 0xFFFF) return; // already resolved

    // 1. Scan DSDT
    uint64_t addr = (fadt->header.revision >= 2 && fadt->x_dsdt) ? fadt->x_dsdt : fadt->dsdt;
    scan_table_for_s5((struct acpi_header*)(addr + hhdm_offset));
    if (slp_typa != 0xFFFF) return;

    // 2. Scan all SSDTs
    int xsdt = !memcmp(acpi_root->signature, "XSDT", 4);
    size_t entsz = xsdt ? 8 : 4;
    int n = (acpi_root->length - sizeof(struct acpi_header)) / entsz;
    uint8_t* p = (uint8_t*)acpi_root + sizeof(struct acpi_header);
    for (int i = 0; i < n; i++) {
        uint64_t phys = xsdt ? ((uint64_t*)p)[i] : ((uint32_t*)p)[i];
        struct acpi_header* h = (struct acpi_header*)(phys + hhdm_offset);
        if (!memcmp(h->signature, "SSDT", 4)) {
            scan_table_for_s5(h);
            if (slp_typa != 0xFFFF) return;
        }
    }
}

// ---- Apple/NVIDIA SPTS/SLPN/SLPT/OEMS vendor SMI quirk ----

static int aml_exec_pts(uint8_t slp_state) {
    uint64_t irq;
    spin_lock_irqsave(&acpi_lock, &irq);

    // Step 1: PS1S=1, PS1E=1 via SPTS or raw IO from SCIO
    aml_obj_t *spts = ns_find("\\", "SPTS");
    if (spts && spts->type == AML_METHOD) {
        call_method1("SPTS", "\\", slp_state);
    } else {
        aml_obj_t *scio = ns_find("\\", "SCIO");
        if (scio && scio->type == AML_INT) {
            uint16_t b = (uint16_t)scio->ival;
            outb(b + 1, inb(b + 1) | 0x80);
            outb(b + 5, inb(b + 5) | 0x80);
        }
    }

    // Step 2: Write SLPN directly via field I/O
    aml_obj_t *slpn = ns_find("\\", "SLPN");
    if (slpn && slpn->type == AML_FIELD) fld_write(slpn, slp_state);

    // Step 3: Write SLPT directly.
    // ECMS is ByteAcc - read RAMB as 4 separate inb() calls, inl() returns garbage.
    aml_obj_t *ramb_fld = ns_find("\\", "RAMB");
    aml_obj_t *slpt_fld = ns_find("\\", "SLPT");
    uint32_t ramb_val = 0;
    if (ramb_fld && ramb_fld->type == AML_FIELD) {
        aml_obj_t *ecms = ns_exact(ramb_fld->field.rgn);
        if (ecms && ecms->type == AML_REGION && ecms->region.space == 1) {
            uint32_t rp = (uint32_t)(ecms->region.base + ramb_fld->field.bit_off / 8);
            ramb_val = (uint32_t)inb((uint16_t)(rp+0))
                     | ((uint32_t)inb((uint16_t)(rp+1)) << 8)
                     | ((uint32_t)inb((uint16_t)(rp+2)) << 16)
                     | ((uint32_t)inb((uint16_t)(rp+3)) << 24);
        }
    }
    if (ramb_val && ramb_val != 0xFFFFFFFFu && slpt_fld && slpt_fld->type == AML_FIELD) {
        uint32_t slpt_off = slpt_fld->field.bit_off / 8;
        volatile uint8_t *sp = (volatile uint8_t*)((uintptr_t)ramb_val + hhdm_offset + slpt_off);
        *sp = slp_state;
    }

    // DEBUG: print readbacks then halt BEFORE SMI fires so LCD stays on.
    // Remove halt() once SLPN=5, SLPT=5, RAMB looks like a valid address.
    {
        if (ramb_val && ramb_val != 0xFFFFFFFFu && slpt_fld && slpt_fld->type == AML_FIELD)
            (void)*(volatile uint8_t*)((uintptr_t)ramb_val + hhdm_offset + slpt_fld->field.bit_off/8);
    }

    // Step 4: OEMS fires ISMI(0x9D) - SMI reads SLPN/SLPT to prepare chipset
    aml_obj_t *oems = ns_find("\\", "OEMS");
    if (!oems || oems->type != AML_METHOD) { spin_unlock_irqrestore(&acpi_lock, irq); return 0; }
    call_method1("OEMS", "\\", slp_state);
    spin_unlock_irqrestore(&acpi_lock, irq);
    return 1;
}

// ---- public API ----

void poweroff(void) {
    if (!fadt) halt();
    parse_s5();
    cli();

    uint8_t sleep_val = (uint8_t)slp_typa; 
    if (sleep_val == 0xFF) sleep_val = 7;
    uint8_t sleep_val_b = (slp_typb == 0 || slp_typb == 0xFFFF) ? sleep_val : (uint8_t)slp_typb;

    struct acpi_gas pm1a = { .address_space_id = 1, .register_bit_width = 16, .address = fadt->pm1a_cnt_blk };
    struct acpi_gas pm1b = { .address_space_id = 1, .register_bit_width = 16, .address = fadt->pm1b_cnt_blk };
    if (fadt->header.revision >= 2 && fadt->x_pm1a_cnt_blk.address) pm1a = fadt->x_pm1a_cnt_blk;
    if (fadt->header.revision >= 2 && fadt->x_pm1b_cnt_blk.address) pm1b = fadt->x_pm1b_cnt_blk;

    // 1. Call \_PTS(5): ACPI spec requires this before entering S5
    aml_obj_t *pts = ns_find("\\", "_PTS");
    if (pts && pts->type == AML_METHOD) {
        call_method1("_PTS", "\\", 5);
    }

    // 2. Platform-specific prep (Apple/NVIDIA SPTS/SLPN/SLPT/OEMS)
    if (ns_find("\\", "SPTS") || ns_find("\\", "SLPN") || ns_find("\\", "RAMB")) {
        aml_exec_pts(sleep_val); 
    }

    // 3. Clear wake status
    if (fadt->pm1a_evt_blk) outw(fadt->pm1a_evt_blk, 0xFFFF);
    if (fadt->pm1b_evt_blk) outw(fadt->pm1b_evt_blk, 0xFFFF);

    // 4. Read-modify-write PM1_CNT: preserve all bits except SLP_TYP and SLP_EN
    uint16_t cur_a = (uint16_t)read_acpi(&pm1a);
    uint16_t val_a = (cur_a & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN))
                   | ((uint16_t)sleep_val << 10) | PM1_CNT_SLP_EN;

    if (pm1b.address) {
        uint16_t cur_b = (uint16_t)read_acpi(&pm1b);
        uint16_t val_b = (cur_b & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN))
                       | ((uint16_t)sleep_val_b << 10) | PM1_CNT_SLP_EN;
        write_acpi(&pm1b, val_b);
    }
    write_acpi(&pm1a, val_a);

    // Retry loop in case the first write doesn't take
    while(1) {
        write_acpi(&pm1a, val_a);
    }
}

void reboot(void) {
    // Disable interrupts first...
    cli();
    // Is there a Reset register?
    if (fadt && fadt->header.revision >= 2 && fadt->reset_reg.address) {
        write_acpi(&fadt->reset_reg, fadt->reset_value);
        for (volatile int d = 0; d < 100000; d++) __asm__ volatile("nop");
    }
    // Seems like there isn't one, let's fallback to the old 80s keyboard reset.
    while (inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    // If the keyboard reset didn't work, let's instead fallback to the PCI hard reset.
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    // If nothing worked, halt.
    halt();
}
