#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <io/pci.h>

#define NVME_CLASS          0x01
#define NVME_SUBCLASS       0x08
#define NVME_PROGIF         0x02
#define NVME_MAX_NAMESPACES 8
#define NVME_QUEUE_DEPTH    64
#define NVME_BLOCK_SIZE     512
#define NVME_MAX_TRANSFER   131072
#define NVME_RW_MAX_PAGES   (NVME_MAX_TRANSFER / 4096)

#define NVME_IO_QID         1
#define NVME_IO_QSIZE       15
#define NVME_NS_MIN_SECTORS 1024
#define NVME_ID_CNS_CTRL    1
#define NVME_ID_CNS_NS      0

#define NVME_PCI_BAR0       0x10
#define NVME_PCI_BAR_MASK   0xFFFFFFF0ULL
#define NVME_PCI_BAR_TYPE   0x06
#define NVME_PCI_BAR_TYPE64 0x04
#define NVME_PCI_BAR_IO     0x01
#define NVME_BAR_SIZE       16384
#define NVME_BAR_ALIGN_MASK 0xF

#define NVME_REG_CAP    0x00
#define NVME_REG_VS     0x08
#define NVME_REG_CC     0x14
#define NVME_REG_CSTS   0x1C
#define NVME_REG_AQA    0x24
#define NVME_REG_ASQ    0x28
#define NVME_REG_ACQ    0x30
#define NVME_REG_CMBLOC 0x38

#define NVME_NVM_IOSQES 6
#define NVME_NVM_IOCQES 4
#define NVME_CC_EN      (1U << 0)
#define NVME_CSTS_RDY   (1U << 0)
#define NVME_CC_IOSQES  (NVME_NVM_IOSQES << 16)
#define NVME_CC_IOCQES  (NVME_NVM_IOCQES << 20)

#define NVME_ADMIN_DELETE_SQ 0x00
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_DELETE_CQ 0x04
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY  0x06

#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ  0x02

#define NVME_CQ_PC  (1U << 0)
#define NVME_CQ_IEN (1U << 1)
#define NVME_SQ_PC  (1U << 0)

#define NVME_QUEUE_QID_SHIFT   0
#define NVME_QUEUE_QSIZE_SHIFT 16
#define NVME_SQ_CQID_SHIFT     16

#define NVME_CAP_MQES_MASK    0xFFFF
#define NVME_CAP_DSTRD_SHIFT  32
#define NVME_CAP_DSTRD_MASK   0xF
#define NVME_CAP_MPSMIN_SHIFT 48
#define NVME_CAP_MPSMIN_MASK  0xF

#define NVME_DOORBELL_BASE 0x1000
#define NVME_DSTRD_UNIT    4

#define NVME_TIMEOUT_POLL  1000000
#define NVME_TIMEOUT_READY 5000

typedef struct {
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_rw_command_t;

typedef struct {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sq_entry_t;

typedef struct {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_cq_entry_t;

typedef struct {
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t cq_phase;
    uint16_t qid;
    uint16_t depth;
    nvme_sq_entry_t *sq;
    nvme_cq_entry_t *cq;
    uint64_t sq_phys;
    uint64_t cq_phys;
    uint64_t sq_doorbell;
    uint64_t cq_doorbell;
} nvme_queue_t;

typedef struct {
    uint64_t cap;
    uint32_t vs;
    uint32_t cc;
    uint32_t csts;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;
    volatile uint8_t *regs;
    nvme_queue_t admin_q;
    nvme_queue_t io_q;
    uint32_t controller_index;
    uint32_t ns_count;
    uint64_t ns_sectors[NVME_MAX_NAMESPACES];
    uint32_t ns_block_size[NVME_MAX_NAMESPACES];
    uint16_t next_cid;
    bool ready;
} nvme_controller_t;

bool has_nvme_device(void);
int get_nvme_namespace_count(void);
bool nvme_device_size(int index, uint64_t *size);
bool make_nvme_ctrl_name(char *name, size_t name_size);
uint64_t read_nvme_ctrl(void *buf, uint64_t count, uint64_t offset, int index);
uint64_t write_nvme_ctrl(const void *buf, uint64_t count, uint64_t offset, int index);
bool make_nvme_disk_name(char *name, size_t name_size, int index);
uint64_t read_nvme_device(void *buf, uint64_t count, uint64_t offset, int index);
uint64_t write_nvme_device(const void *buf, uint64_t count, uint64_t offset, int index);
int64_t read_nvme_blocks(uint32_t ns, uint64_t lba, void *buf, uint64_t count);
int64_t write_nvme_blocks(uint32_t ns, uint64_t lba, const void *buf, uint64_t count);
void init_nvme(pci_device_t *dev);
