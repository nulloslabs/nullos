#pragma once

#include <freestanding/stdint.h>
#include <limine/limine.h>

#define SCI_EN (1 << 0)
#define SLP_EN (1 << 13)

struct acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct rsdp_descriptor {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_gas {
    uint8_t address_space_id; // 0 = MMIO, 1 = I/O
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct fadt_descriptor {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;
    struct acpi_gas x_pm1b_cnt_blk;
    struct acpi_gas x_pm2_cnt_blk;
    struct acpi_gas x_pm_tmr_blk;
    struct acpi_gas x_gpe0_blk;
    struct acpi_gas x_gpe1_blk;
} __attribute__((packed));

#define AML_NS_MAX 4096
#define AML_NAME_MAX  128
#define AML_DEPTH_MAX 12

typedef struct {
    uint64_t args[7];
    uint64_t locals[8];
    char scope[AML_NAME_MAX];
    int depth;
} aml_ctx_t;

typedef enum {
    AML_NONE = 0, AML_INT, AML_METHOD, AML_REGION, AML_FIELD, AML_BUFFER
} aml_type_t;

// ACPI resource descriptor types (from _CRS evaluation)
typedef enum {
    ACPI_RES_IO = 0,        // I/O port range
    ACPI_RES_MMIO,          // Memory-mapped I/O (AddressSpace)
    ACPI_RES_IRQ,           // IRQ number
    ACPI_RES_DMA,           // DMA channel
} acpi_res_type_t;

// Single resource entry
typedef struct {
    acpi_res_type_t type;
    uint8_t         io_space_id;    // 0 = Memory, 1 = SystemIO
    uint64_t        base;           // Base address (IO port or physical MMIO)
    uint64_t        length;         // Length in bytes
    uint32_t        irq;            // IRQ number (for ACPI_RES_IRQ)
    int             valid;          // 1 if this entry is valid
} acpi_resource_t;

#define ACPI_MAX_RESOURCES 16

// ACPI device object (represents a device in the namespace)
typedef struct {
    char       path[AML_NAME_MAX];   // Full AML path (e.g. "\_SB.PCI0.EHC1")
    uint64_t   hid;                  // _HID value (string encoded as int, or 0)
    char       hid_str[AML_NAME_MAX]; // _HID as string (e.g. "PNP0A03")
    uint64_t   adr;                  // _ADR value (for PCI: seg<<16 | bus<<8 | dev<<3 | func<<0)
    uint64_t   uid;                  // _UID value
    int        has_crs;              // 1 if _CRS exists
    int        has_hid;              // 1 if _HID exists
    int        has_adr;              // 1 if _ADR exists
} acpi_device_t;

#define ACPI_MAX_DEVICES 256

typedef struct {
    acpi_device_t  devices[ACPI_MAX_DEVICES];
    int            count;
} acpi_device_registry_t;

typedef struct {
    acpi_resource_t resources[ACPI_MAX_RESOURCES];
    int             count;
} acpi_resource_list_t;

typedef struct {
    char       path[AML_NAME_MAX];
    aml_type_t type;
    union {
        uint64_t ival;
        struct { uint8_t *body; uint32_t blen; uint8_t argc; } method;
        struct {
            uint8_t  space;                  // 0=Mem 1=IO
            int      dyn;                    // base is a runtime field ref
            uint64_t base;
            char     base_fld[AML_NAME_MAX]; // if dyn: absolute path of base field
            uint32_t len;
        } region;
        struct {
            char     rgn[AML_NAME_MAX];      // absolute path of owning region
            uint32_t bit_off;
            uint32_t bit_wid;
        } field;
        struct { uint8_t *data; uint32_t len; } buffer;
    };
} aml_obj_t;

// Result types
#define RET_VAL  0  // normal integer result
#define RET_VOID 1  // void (no value)
#define RET_RET  2  // return statement hit

typedef struct { uint64_t v; int t; } aml_val_t;
#define V(x)  ((aml_val_t){.v=(x),.t=RET_VAL})
#define VOID  ((aml_val_t){.t=RET_VOID})
#define RET(x)((aml_val_t){.v=(x),.t=RET_RET})

#define PM1_CNT_BM_RLD       (1u << 1)
#define PM1_CNT_SLP_TYP_MASK (0x7u << 10)
#define PM1_CNT_SLP_EN       (1u << 13)
#define PM1_CNT_WRITE_MASK   (PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN | PM1_CNT_BM_RLD)

void evaluate_acpi_crs(const char *device_path, acpi_resource_list_t *out);
void enumerate_acpi_devices(void);
const acpi_device_t* find_acpi_pci_device(uint8_t bus, uint8_t dev, uint8_t func);
int get_acpi_usb_resources(uint8_t bus, uint8_t dev, uint8_t func, acpi_resource_list_t *out);
int get_acpi_device_count(void);
const acpi_device_registry_t* get_acpi_devices(void);
void* find_acpi_table(const char* sig);
void init_acpi(void);
void poweroff(void);
void reboot(void);