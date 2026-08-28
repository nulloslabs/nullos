#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/pci.h>
#include <io/nvme.h>
#include <io/io.h>
#include <io/time.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

static nvme_controller_t g_nvme_ctrl = {0};
static bool g_has_nvme = false;
static spinlock_t g_nvme_lock = SPINLOCK_INIT;

static inline uint32_t read_nvme_reg32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void write_nvme_reg32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(base + off) = val;
}

static inline uint64_t read_nvme_reg64(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint64_t*)(base + off);
}

static inline void write_nvme_reg64(volatile uint8_t *base, uint32_t off, uint64_t val) {
    *(volatile uint64_t*)(base + off) = val;
}

static bool is_nvme_bar_valid(uint64_t bar, uint64_t size) {
    if (!bar || !size) return false;
    if (bar & NVME_BAR_ALIGN_MASK) return false;
    return true;
}

static void* map_nvme_bar(pci_device_t *dev, size_t *out_pages) {
    uint32_t low = read_pci(dev->bus, dev->dev, dev->func, NVME_PCI_BAR0);
    uint32_t high = read_pci(dev->bus, dev->dev, dev->func, NVME_PCI_BAR0 + 4);
    uint64_t bar = low & NVME_PCI_BAR_MASK;
    if ((low & NVME_PCI_BAR_TYPE) == NVME_PCI_BAR_TYPE64) bar |= (uint64_t)high << 32;
    if (low & NVME_PCI_BAR_IO) return 0;
    uint64_t size = NVME_BAR_SIZE;
    if (!is_nvme_bar_valid(bar, size)) return 0;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *virt = vmap_mmio(bar, pages);
    if (!virt) return 0;
    if (out_pages) *out_pages = pages;
    return virt;
}

static int alloc_nvme_queue(nvme_queue_t *q, uint16_t depth, uint16_t qid) {
    // alloc SQ and CQ pages
    size_t sq_bytes = depth * sizeof(nvme_sq_entry_t);
    size_t cq_bytes = depth * sizeof(nvme_cq_entry_t);
    size_t sq_pages = (sq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t cq_pages = (cq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!sq_pages) sq_pages = 1;
    if (!cq_pages) cq_pages = 1;
    void *sq_phys = pmalloc();
    void *cq_phys = pmalloc();
    if (!sq_phys) return -ENOMEM;
    if (!cq_phys) { pfree(sq_phys); return -ENOMEM; }
    memset(phys_to_virt((uint64_t)sq_phys), 0, PAGE_SIZE);
    memset(phys_to_virt((uint64_t)cq_phys), 0, PAGE_SIZE);
    q->sq_phys = (uint64_t)sq_phys;
    q->cq_phys = (uint64_t)cq_phys;
    q->sq = phys_to_virt(q->sq_phys);
    q->cq = phys_to_virt(q->cq_phys);
    q->depth = depth;
    q->qid = qid;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->cq_phase = 1;
    // doorbell stride from CAP.DSTRD
    uint64_t cap = g_nvme_ctrl.cap;
    uint32_t dstrd = (cap >> NVME_CAP_DSTRD_SHIFT) & NVME_CAP_DSTRD_MASK;
    uint64_t stride = (uint64_t)NVME_DSTRD_UNIT << dstrd;
    q->sq_doorbell = NVME_DOORBELL_BASE + (uint64_t)qid * stride * 2;
    q->cq_doorbell = NVME_DOORBELL_BASE + (uint64_t)qid * stride * 2 + stride;
    return 0;
}

static int poll_nvme_cq(nvme_queue_t *q, uint16_t cid, uint32_t *result) {
    // poll for completion
    for (int t = 0; t < NVME_TIMEOUT_POLL; t++) {
        volatile nvme_cq_entry_t *e = &q->cq[q->cq_head];
        __asm__ volatile("" ::: "memory");
        uint16_t sf = e->status & 0xFFFF;
        uint16_t phase = sf & 0x1;
        if (phase != q->cq_phase) { __asm__ volatile("pause"); continue; }
        if (e->cid != cid) { __asm__ volatile("pause"); continue; }
        if (result) *result = e->result;
        uint16_t status = sf >> 1;
        uint16_t sc = status & 0xFF;
        uint16_t sct = (status >> 8) & 0x7;
        q->cq_head++;
        if (q->cq_head >= q->depth) {
            q->cq_head = 0;
            q->cq_phase ^= 1;
        }
        write_nvme_reg32(g_nvme_ctrl.regs, q->cq_doorbell, q->cq_head);
        if (sct || sc) return -EIO;
        return 0;
    }
    log("nvme: completion queue poll timed out\n");
    return -ETIMEDOUT;
}

static int submit_nvme_admin(nvme_controller_t *c, nvme_sq_entry_t *cmd, uint32_t *result) {
    // submit to admin queue
    uint16_t cid = c->next_cid++;
    if (c->next_cid == 0) c->next_cid = 1;
    cmd->cid = cid;
    nvme_queue_t *q = &c->admin_q;
    uint16_t tail = q->sq_tail;
    q->sq[tail] = *cmd;
    __asm__ volatile("mfence" ::: "memory");
    q->sq_tail++;
    if (q->sq_tail >= q->depth) q->sq_tail = 0;
    write_nvme_reg32(c->regs, q->sq_doorbell, q->sq_tail);
    return poll_nvme_cq(q, cid, result);
}

static int identify_nvme_controller(nvme_controller_t *c) {
    // alloc identify buffer 4K
    void *phys = pmalloc();
    if (!phys) return -ENOMEM;
    void *virt = phys_to_virt((uint64_t)phys);
    memset(virt, 0, 4096);
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 0;
    cmd.prp1 = (uint64_t)phys;
    cmd.cdw10 = NVME_ID_CNS_CTRL;
    int st = submit_nvme_admin(c, &cmd, 0);
    if (st < 0) { pfree(phys); return st; }
    uint32_t *id = virt;
    // NN (number of namespaces) is at byte offset 516 (dword 129)
    uint32_t nn = id[516 / 4];
    if (nn > NVME_MAX_NAMESPACES) nn = NVME_MAX_NAMESPACES;
    c->ns_count = nn;
    pfree(phys);
    // identify each namespace
    for (uint32_t ns = 1; ns <= c->ns_count; ns++) {
        void *p = pmalloc();
        if (!p) continue;
        void *v = phys_to_virt((uint64_t)p);
        memset(v, 0, PAGE_SIZE);
        nvme_sq_entry_t c2 = {0};
        c2.opcode = NVME_ADMIN_IDENTIFY;
        c2.nsid = ns;
        c2.prp1 = (uint64_t)p;
        c2.cdw10 = NVME_ID_CNS_NS;
        int s = submit_nvme_admin(c, &c2, 0);
        if (s == 0) {
            uint8_t *id = v;
            uint64_t nlb = *(uint64_t *)id;
            // NSZE == 0 means the namespace is not active; skip it
            if (nlb != 0) {
                uint8_t flbas = id[26] & 0xF;
                uint8_t lbads = id[128 + flbas * 4 + 2]; // LBADS is byte 2 of each 4-byte LBA format entry
                uint32_t block = lbads ? (1U << lbads) : NVME_BLOCK_SIZE;
                c->ns_sectors[ns - 1] = nlb;
                c->ns_block_size[ns - 1] = block;
            }
        }
        pfree(p);
    }
    return 0;
}

static int wait_nvme_ready(nvme_controller_t *c, bool ready) {
    // wait CSTS.RDY
    for (int t = 0; t < NVME_TIMEOUT_READY; t++) {
        uint32_t csts = read_nvme_reg32(c->regs, NVME_REG_CSTS);
        bool r = csts & NVME_CSTS_RDY;
        if (r == ready) return 0;
        sleep(1);
    }
    return -ETIMEDOUT;
}

static int setup_nvme_io_queue(nvme_controller_t *c) {
    // create paired IO CQ then SQ, single qid
    uint16_t qid = NVME_IO_QID;
    uint16_t qsize = NVME_IO_QSIZE;
    nvme_queue_t *q = &c->io_q;
    int st = alloc_nvme_queue(q, qsize + 1, qid);
    if (st < 0) return st;
    // create CQ
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_ADMIN_CREATE_CQ;
    cmd.prp1 = q->cq_phys;
    cmd.cdw10 = (uint32_t)qid | (uint32_t)qsize << NVME_QUEUE_QSIZE_SHIFT;
    cmd.cdw11 = NVME_CQ_IEN | NVME_CQ_PC;
    st = submit_nvme_admin(c, &cmd, 0);
    if (st < 0) { log("nvme: couldn't create completion queue\n"); return st; }
    // create SQ bound to CQ
    nvme_sq_entry_t cmd2 = {0};
    cmd2.opcode = NVME_ADMIN_CREATE_SQ;
    cmd2.prp1 = q->sq_phys;
    cmd2.cdw10 = (uint32_t)qid | (uint32_t)qsize << NVME_QUEUE_QSIZE_SHIFT;
    cmd2.cdw11 = (uint32_t)qid << NVME_SQ_CQID_SHIFT | NVME_SQ_PC;
    st = submit_nvme_admin(c, &cmd2, 0);
    if (st < 0) { log("nvme: couldn't create completion queue\n"); return st; }
    return 0;
}

bool has_nvme_device(void) {
    return g_has_nvme && g_nvme_ctrl.ready;
}

int get_nvme_namespace_count(void) {
    if (!g_has_nvme) return 0;
    return g_nvme_ctrl.ns_count;
}

bool nvme_device_size(int index, uint64_t *size) {
    if (index < 0 || !size) return false;
    if (!g_has_nvme || index >= (int)g_nvme_ctrl.ns_count) return false;
    uint64_t sectors = g_nvme_ctrl.ns_sectors[index];
    if (!sectors) return false;
    uint32_t block = g_nvme_ctrl.ns_block_size[index];
    if (!block) block = NVME_BLOCK_SIZE;
    *size = sectors * block;
    return true;
}

bool make_nvme_ctrl_name(char *name, size_t name_size) {
    if (!name || !g_has_nvme) return false;
    uint32_t ctrl = g_nvme_ctrl.controller_index;
    char buf[16];
    size_t len = 0;
    if (ctrl >= 10) return false;
    buf[len++] = 'n';
    buf[len++] = 'v';
    buf[len++] = 'm';
    buf[len++] = 'e';
    buf[len++] = '0' + ctrl;
    buf[len] = '\0';
    if (len + 1 > name_size) return false;
    memcpy(name, buf, len + 1);
    return true;
}

uint64_t read_nvme_ctrl(void *buf, uint64_t count, uint64_t offset, int index) {
    (void)buf; (void)count; (void)offset; (void)index;
    return 0;
}

uint64_t write_nvme_ctrl(const void *buf, uint64_t count, uint64_t offset, int index) {
    (void)buf; (void)count; (void)offset; (void)index;
    return 0;
}

bool make_nvme_disk_name(char *name, size_t name_size, int index) {
    if (!name || !g_has_nvme || index < 0 || index >= (int)g_nvme_ctrl.ns_count) return false;
    uint32_t ctrl = g_nvme_ctrl.controller_index;
    uint32_t nsid = (uint32_t)index + 1;
    char buf[16];
    size_t len = 0;
    if (ctrl >= 10 || nsid >= 10) return false;
    buf[len++] = 'n';
    buf[len++] = 'v';
    buf[len++] = 'm';
    buf[len++] = 'e';
    buf[len++] = '0' + ctrl;
    buf[len++] = 'n';
    buf[len++] = '0' + nsid;
    buf[len] = '\0';
    if (len + 1 > name_size) return false;
    memcpy(name, buf, len + 1);
    return true;
}

uint64_t read_nvme_device(void *buf, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= (int)g_nvme_ctrl.ns_count) return (uint64_t)-ENODEV;
    if (!count) return 0;
    if (!buf) return (uint64_t)-EINVAL;
    uint32_t block = g_nvme_ctrl.ns_block_size[index];
    if (!block) block = NVME_BLOCK_SIZE;
    uint64_t total_size = g_nvme_ctrl.ns_sectors[index] * block;
    if (offset >= total_size) return 0;
    if (count > total_size - offset) count = total_size - offset;

    uint8_t sector_buf[4096];
    if (block > sizeof(sector_buf)) return (uint64_t)-EINVAL;

    uint64_t completed = 0;
    while (completed < count) {
        uint64_t pos = offset + completed;
        uint64_t lba = pos / block;
        uint32_t sector_off = pos % block;
        uint64_t remaining = count - completed;

        if (sector_off == 0 && remaining >= block) {
            uint64_t num_blocks = remaining / block;
            uint64_t max_blocks = NVME_MAX_TRANSFER / block;
            if (num_blocks > max_blocks) num_blocks = max_blocks;
            int64_t ret = read_nvme_blocks(index + 1, lba, (uint8_t *)buf + completed, num_blocks);
            if (ret < 0) return (uint64_t)ret;
            completed += (uint64_t)ret;
            continue;
        }

        int64_t ret = read_nvme_blocks(index + 1, lba, sector_buf, 1);
        if (ret < 0) return (uint64_t)ret;
        uint64_t bytes = block - sector_off;
        if (bytes > remaining) bytes = remaining;
        memcpy((uint8_t *)buf + completed, sector_buf + sector_off, bytes);
        completed += bytes;
    }
    return completed;
}

uint64_t write_nvme_device(const void *buf, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= (int)g_nvme_ctrl.ns_count) return (uint64_t)-ENODEV;
    if (!count) return 0;
    if (!buf) return (uint64_t)-EINVAL;
    uint32_t block = g_nvme_ctrl.ns_block_size[index];
    if (!block) block = NVME_BLOCK_SIZE;
    uint64_t total_size = g_nvme_ctrl.ns_sectors[index] * block;
    if (offset >= total_size) return 0;
    if (count > total_size - offset) count = total_size - offset;

    uint8_t sector_buf[4096];
    if (block > sizeof(sector_buf)) return (uint64_t)-EINVAL;

    uint64_t completed = 0;
    while (completed < count) {
        uint64_t pos = offset + completed;
        uint64_t lba = pos / block;
        uint32_t sector_off = pos % block;
        uint64_t remaining = count - completed;

        if (sector_off == 0 && remaining >= block) {
            uint64_t num_blocks = remaining / block;
            uint64_t max_blocks = NVME_MAX_TRANSFER / block;
            if (num_blocks > max_blocks) num_blocks = max_blocks;
            int64_t ret = write_nvme_blocks(index + 1, lba, (const uint8_t *)buf + completed, num_blocks);
            if (ret < 0) return (uint64_t)ret;
            completed += (uint64_t)ret;
            continue;
        }

        int64_t ret = read_nvme_blocks(index + 1, lba, sector_buf, 1);
        if (ret < 0) return (uint64_t)ret;
        uint64_t bytes = block - sector_off;
        if (bytes > remaining) bytes = remaining;
        memcpy(sector_buf + sector_off, (const uint8_t *)buf + completed, bytes);
        ret = write_nvme_blocks(index + 1, lba, sector_buf, 1);
        if (ret < 0) return (uint64_t)ret;
        completed += bytes;
    }
    return completed;
}

int64_t read_nvme_blocks(uint32_t ns, uint64_t lba, void *buf, uint64_t count) {
    if (!g_has_nvme || !g_nvme_ctrl.ready) return -ENODEV;
    if (!buf && count) return -EINVAL;
    if (ns == 0 || ns > g_nvme_ctrl.ns_count) return -EINVAL;
    uint64_t sectors = g_nvme_ctrl.ns_sectors[ns - 1];
    uint32_t block = g_nvme_ctrl.ns_block_size[ns - 1];
    if (!block) block = NVME_BLOCK_SIZE;
    if (lba >= sectors) return 0;
    if (count > sectors - lba) count = sectors - lba;
    if (count == 0) return 0;
    uint64_t bytes = count * block;
    if (bytes > NVME_MAX_TRANSFER) count = NVME_MAX_TRANSFER / block;
    if (count == 0) return -EINVAL;
    bytes = count * block;

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *data_pages[NVME_RW_MAX_PAGES];
    void *list_page = 0;
    memset(data_pages, 0, sizeof(data_pages));

    for (size_t i = 0; i < pages; i++) {
        data_pages[i] = pmalloc();
        if (!data_pages[i]) {
            for (size_t j = 0; j < i; j++) pfree(data_pages[j]);
            return -ENOMEM;
        }
    }

    if (pages > 2) {
        list_page = pmalloc();
        if (!list_page) {
            for (size_t j = 0; j < pages; j++) pfree(data_pages[j]);
            return -ENOMEM;
        }
    }

    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_CMD_READ;
    cmd.nsid = ns;
    if (pages == 1) {
        cmd.prp1 = (uint64_t)data_pages[0];
    } else if (pages == 2) {
        // exactly two pages: PRP1 is page 0, PRP2 is page 1 directly
        cmd.prp1 = (uint64_t)data_pages[0];
        cmd.prp2 = (uint64_t)data_pages[1];
    } else {
        // more than two pages: PRP1 is page 0, PRP2 points to PRP list containing pages 1..N-1
        uint64_t *list = phys_to_virt((uint64_t)list_page);
        memset(list, 0, PAGE_SIZE);
        for (size_t i = 1; i < pages; i++) list[i - 1] = (uint64_t)data_pages[i];
        cmd.prp1 = (uint64_t)data_pages[0];
        cmd.prp2 = (uint64_t)list_page;
    }
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = (count - 1) & 0xFFFF;

    nvme_controller_t *c = &g_nvme_ctrl;
    nvme_queue_t *q = &c->io_q;
    uint64_t flags;
    spin_lock_irqsave(&g_nvme_lock, &flags);
    uint16_t cid = c->next_cid++;
    if (c->next_cid == 0) c->next_cid = 1;
    cmd.cid = cid;
    uint16_t tail = q->sq_tail;
    q->sq[tail] = cmd;
    __asm__ volatile("mfence" ::: "memory");
    q->sq_tail++;
    if (q->sq_tail >= q->depth) q->sq_tail = 0;
    write_nvme_reg32(c->regs, q->sq_doorbell, q->sq_tail);
    int st = poll_nvme_cq(q, cid, 0);
    spin_unlock_irqrestore(&g_nvme_lock, flags);

    if (st == 0) {
        for (size_t i = 0; i < pages; i++) {
            uint64_t chunk = bytes - i * PAGE_SIZE;
            if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
            memcpy((uint8_t *)buf + i * PAGE_SIZE, phys_to_virt((uint64_t)data_pages[i]), chunk);
        }
    }

    if (list_page) pfree(list_page);
    for (size_t i = 0; i < pages; i++) {
        if (data_pages[i]) pfree(data_pages[i]);
    }
    if (st < 0) return st;
    return (int64_t)(count * block);
}

int64_t write_nvme_blocks(uint32_t ns, uint64_t lba, const void *buf, uint64_t count) {
    if (!g_has_nvme || !g_nvme_ctrl.ready) return -ENODEV;
    if (!buf && count) return -EINVAL;
    if (ns == 0 || ns > g_nvme_ctrl.ns_count) return -EINVAL;
    uint64_t sectors = g_nvme_ctrl.ns_sectors[ns - 1];
    uint32_t block = g_nvme_ctrl.ns_block_size[ns - 1];
    if (!block) block = NVME_BLOCK_SIZE;
    if (lba >= sectors) return 0;
    if (count > sectors - lba) count = sectors - lba;
    if (count == 0) return 0;
    uint64_t bytes = count * block;
    if (bytes > NVME_MAX_TRANSFER) count = NVME_MAX_TRANSFER / block;
    if (count == 0) return -EINVAL;
    bytes = count * block;

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *data_pages[NVME_RW_MAX_PAGES];
    void *list_page = 0;
    memset(data_pages, 0, sizeof(data_pages));

    for (size_t i = 0; i < pages; i++) {
        data_pages[i] = pmalloc();
        if (!data_pages[i]) {
            for (size_t j = 0; j < i; j++) pfree(data_pages[j]);
            return -ENOMEM;
        }
        uint64_t chunk = bytes - i * PAGE_SIZE;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        memcpy(phys_to_virt((uint64_t)data_pages[i]), (uint8_t *)buf + i * PAGE_SIZE, chunk);
    }

    if (pages > 2) {
        list_page = pmalloc();
        if (!list_page) {
            for (size_t j = 0; j < pages; j++) pfree(data_pages[j]);
            return -ENOMEM;
        }
    }

    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_CMD_WRITE;
    cmd.nsid = ns;
    if (pages == 1) {
        cmd.prp1 = (uint64_t)data_pages[0];
    } else if (pages == 2) {
        // exactly two pages: PRP1 is page 0, PRP2 is page 1 directly
        cmd.prp1 = (uint64_t)data_pages[0];
        cmd.prp2 = (uint64_t)data_pages[1];
    } else {
        // more than two pages: PRP1 is page 0, PRP2 points at a PRP list page
        uint64_t *list = phys_to_virt((uint64_t)list_page);
        memset(list, 0, PAGE_SIZE);
        for (size_t i = 1; i < pages; i++) list[i - 1] = (uint64_t)data_pages[i];
        cmd.prp1 = (uint64_t)data_pages[0];
        cmd.prp2 = (uint64_t)list_page;
    }
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = (count - 1) & 0xFFFF;

    nvme_controller_t *c = &g_nvme_ctrl;
    nvme_queue_t *q = &c->io_q;
    uint64_t flags;
    spin_lock_irqsave(&g_nvme_lock, &flags);
    uint16_t cid = c->next_cid++;
    if (c->next_cid == 0) c->next_cid = 1;
    cmd.cid = cid;
    uint16_t tail = q->sq_tail;
    q->sq[tail] = cmd;
    __asm__ volatile("mfence" ::: "memory");
    q->sq_tail++;
    if (q->sq_tail >= q->depth) q->sq_tail = 0;
    write_nvme_reg32(c->regs, q->sq_doorbell, q->sq_tail);
    int st = poll_nvme_cq(q, cid, 0);
    spin_unlock_irqrestore(&g_nvme_lock, flags);

    if (list_page) pfree(list_page);
    for (size_t i = 0; i < pages; i++) {
        if (data_pages[i]) pfree(data_pages[i]);
    }
    if (st < 0) return st;
    return (int64_t)(count * block);
}

void init_nvme(pci_device_t *dev) {
    if (!dev) return;
    if (g_has_nvme) return;
    set_pci_d0(dev);
    // enable bus master and mem space
    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x06);
    size_t pages = 0;
    void *regs = map_nvme_bar(dev, &pages);
    if (!regs) { log("nvme: couldn't map bar\n"); return; }
    nvme_controller_t *c = &g_nvme_ctrl;
    memset(c, 0, sizeof(*c));
    c->regs = regs;
    c->cap = read_nvme_reg64(c->regs, NVME_REG_CAP);
    // disable first
    uint32_t cc = read_nvme_reg32(c->regs, NVME_REG_CC);
    cc &= ~NVME_CC_EN;
    write_nvme_reg32(c->regs, NVME_REG_CC, cc);
    if (wait_nvme_ready(c, false) < 0) { log("nvme: couldn't disable nvme\n"); return; }
    // alloc admin queues 64 depth
    if (alloc_nvme_queue(&c->admin_q, NVME_QUEUE_DEPTH, 0) < 0) { log("nvme: couldn't allocate for queue\n"); return; }
    write_nvme_reg32(c->regs, NVME_REG_AQA, ((uint32_t)c->admin_q.depth - 1) << 0 | ((uint32_t)c->admin_q.depth - 1) << 16);
    write_nvme_reg64(c->regs, NVME_REG_ASQ, c->admin_q.sq_phys);
    write_nvme_reg64(c->regs, NVME_REG_ACQ, c->admin_q.cq_phys);
    // enable
    cc = 0;
    cc |= NVME_CC_IOSQES | NVME_CC_IOCQES | NVME_CC_EN;
    write_nvme_reg32(c->regs, NVME_REG_CC, cc);
    if (wait_nvme_ready(c, true) < 0) { log("nvme: couldn't enable drive\n"); return; }
    c->next_cid = 1;
    if (identify_nvme_controller(c) < 0) { log("nvme: couldn't identify drive\n"); return; }
    if (setup_nvme_io_queue(c) < 0) { log("nvme: couldn't setup io queue\n"); return; }
    c->ready = true;
    g_has_nvme = true;
    log("nvme: initialized nvme\n");
}
