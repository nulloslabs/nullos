#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <main/limine_req.h>
#include <main/log.h>
#include <main/spinlocks.h>
#include <main/string.h>
#include <io/fb.h>
#include <io/pci.h>
#include <io/terminal.h>
#include <io/time.h>
#include <io/virtio_gpu.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

static volatile virtio_pci_common_cfg_t *virtio_gpu_common = NULL;
static volatile uint8_t *virtio_gpu_notify_base = NULL;
static uint32_t virtio_gpu_notify_length = 0;
static uint32_t virtio_gpu_notify_multiplier = 0;
static virtio_gpu_queue_t virtio_gpu_control_queue = {0};
static spinlock_t virtio_gpu_lock = SPINLOCK_INIT;
static uint64_t virtio_gpu_command_phys = 0;
static uint64_t virtio_gpu_response_phys = 0;
static void *virtio_gpu_command = NULL;
static void *virtio_gpu_response = NULL;
static uint64_t virtio_gpu_update_commands_phys = 0;
static uint64_t virtio_gpu_update_responses_phys = 0;
static void *virtio_gpu_update_commands = NULL;
static void *virtio_gpu_update_responses = NULL;
static virtio_gpu_update_slot_t virtio_gpu_update_slots[VIRTIO_GPU_UPDATE_SLOT_COUNT] = {0};
static uint64_t virtio_gpu_framebuffer_phys = 0;
static uint64_t virtio_gpu_framebuffer_allocation_size = 0;
static uint32_t virtio_gpu_width = 0;
static uint32_t virtio_gpu_height = 0;
static uint32_t virtio_gpu_scanout = 0;
static uint32_t virtio_gpu_resource_id = 0;
static uint32_t virtio_gpu_next_resource_id = VIRTIO_GPU_FIRST_RESOURCE_ID;
static bool virtio_gpu_transport_ready = false;
static bool virtio_gpu_ready = false;

static uint64_t get_pci_bar_address(pci_device_t *dev, uint8_t bar_index) {
    if (bar_index >= 6) return 0;
    uint8_t offset = 0x10 + bar_index * 4;
    uint32_t bar = read_pci(dev->bus, dev->dev, dev->func, offset);
    if (bar & 1) return 0;
    uint32_t type = (bar >> 1) & 3;
    uint64_t address = bar & 0xFFFFFFF0u;
    if (type == 2) {
        if (bar_index >= 5) return 0;
        address |= (uint64_t)read_pci(dev->bus, dev->dev, dev->func, offset + 4) << 32;
    } else if (type != 0) {
        return 0;
    }
    return address;
}

static void *map_virtio_capability(pci_device_t *dev, const virtio_pci_cap_t *cap) {
    uint64_t bar = get_pci_bar_address(dev, cap->bar);
    if (!bar || !cap->length || cap->offset > UINT64_MAX - bar) return NULL;
    uint64_t address = bar + cap->offset;
    uint64_t page_offset = address & (PAGE_SIZE - 1);
    if (cap->length > UINT64_MAX - page_offset) return NULL;
    uint64_t pages = (page_offset + cap->length + PAGE_SIZE - 1) / PAGE_SIZE;
    return vmap_mmio(address, pages);
}

static bool find_virtio_capabilities(pci_device_t *dev) {
    uint32_t status = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    if (!(status & (1U << 20))) return false;
    uint8_t pointer = read_pci(dev->bus, dev->dev, dev->func, 0x34) & 0xFC;
    for (int count = 0; pointer && count < 48; count++) {
        uint32_t first = read_pci(dev->bus, dev->dev, dev->func, pointer);
        uint8_t next = (first >> 8) & 0xFC;
        uint8_t length = (first >> 16) & 0xFF;
        uint8_t type = first >> 24;
        if ((first & 0xFF) == VIRTIO_PCI_CAP_ID && length >= sizeof(virtio_pci_cap_t)) {
            virtio_pci_cap_t cap = {
                .cap_vndr = first & 0xFF,
                .cap_next = next,
                .cap_len = length,
                .cfg_type = type,
                .bar = read_pci(dev->bus, dev->dev, dev->func, pointer + 4) & 0xFF,
                .offset = read_pci(dev->bus, dev->dev, dev->func, pointer + 8),
                .length = read_pci(dev->bus, dev->dev, dev->func, pointer + 12),
            };
            if (type == VIRTIO_PCI_CAP_COMMON_CFG && cap.length >= sizeof(virtio_pci_common_cfg_t)) {
                virtio_gpu_common = map_virtio_capability(dev, &cap);
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG && length >= sizeof(virtio_pci_notify_cap_t)) {
                virtio_gpu_notify_base = map_virtio_capability(dev, &cap);
                virtio_gpu_notify_length = cap.length;
                virtio_gpu_notify_multiplier = read_pci(dev->bus, dev->dev, dev->func, pointer + 16);
            }
        }
        pointer = next;
    }
    return virtio_gpu_common && virtio_gpu_notify_base && virtio_gpu_notify_multiplier;
}

static void set_virtio_gpu_status(uint8_t status) {
    virtio_gpu_common->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static bool negotiate_virtio_gpu_features(void) {
    set_virtio_gpu_status(0);
    set_virtio_gpu_status(VIRTIO_STATUS_ACKNOWLEDGE);
    set_virtio_gpu_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    virtio_gpu_common->device_feature_select = VIRTIO_F_VERSION_1 / 32;
    if (!(virtio_gpu_common->device_feature & (1U << (VIRTIO_F_VERSION_1 % 32)))) return false;
    virtio_gpu_common->driver_feature_select = 0;
    virtio_gpu_common->driver_feature = 0;
    virtio_gpu_common->driver_feature_select = VIRTIO_F_VERSION_1 / 32;
    virtio_gpu_common->driver_feature = 1U << (VIRTIO_F_VERSION_1 % 32);
    set_virtio_gpu_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    return virtio_gpu_common->device_status & VIRTIO_STATUS_FEATURES_OK;
}

static bool allocate_virtio_gpu_queue_page(uint64_t *phys, void **virt) {
    void *page = pmalloc();
    if (!page) return false;
    *phys = (uint64_t)page;
    *virt = phys_to_virt(*phys);
    memset(*virt, 0, PAGE_SIZE);
    return true;
}

static bool initialize_virtio_gpu_control_queue(void) {
    virtio_gpu_common->queue_select = VIRTIO_GPU_CONTROL_QUEUE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16_t size = virtio_gpu_common->queue_size;
    if (size < VIRTIO_GPU_QUEUE_SIZE || virtio_gpu_common->queue_enable) return false;
    size = VIRTIO_GPU_QUEUE_SIZE;

    void *desc;
    void *avail;
    void *used;
    if (!allocate_virtio_gpu_queue_page(&virtio_gpu_control_queue.desc_phys, &desc)) return false;
    if (!allocate_virtio_gpu_queue_page(&virtio_gpu_control_queue.avail_phys, &avail)) return false;
    if (!allocate_virtio_gpu_queue_page(&virtio_gpu_control_queue.used_phys, &used)) return false;
    virtio_gpu_control_queue.desc = desc;
    virtio_gpu_control_queue.avail = avail;
    virtio_gpu_control_queue.used = used;
    virtio_gpu_control_queue.size = size;
    virtio_gpu_control_queue.last_used_idx = 0;
    virtio_gpu_control_queue.avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    virtio_gpu_common->queue_size = size;
    virtio_gpu_common->queue_desc = virtio_gpu_control_queue.desc_phys;
    virtio_gpu_common->queue_driver = virtio_gpu_control_queue.avail_phys;
    virtio_gpu_common->queue_device = virtio_gpu_control_queue.used_phys;
    uint64_t notify_offset = (uint64_t)virtio_gpu_common->queue_notify_off * virtio_gpu_notify_multiplier;
    if (notify_offset > virtio_gpu_notify_length || sizeof(uint16_t) > virtio_gpu_notify_length - notify_offset) return false;
    virtio_gpu_control_queue.notify = (volatile uint16_t *)(virtio_gpu_notify_base + notify_offset);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    virtio_gpu_common->queue_enable = 1;
    return true;
}

static void reap_virtio_gpu_updates(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (virtio_gpu_control_queue.last_used_idx != virtio_gpu_control_queue.used->idx) {
        uint16_t used_index = virtio_gpu_control_queue.last_used_idx % virtio_gpu_control_queue.size;
        uint32_t descriptor = virtio_gpu_control_queue.used->ring[used_index].id;
        if (descriptor >= VIRTIO_GPU_SYNC_DESC_COUNT) {
            uint32_t slot = (descriptor - VIRTIO_GPU_SYNC_DESC_COUNT) / VIRTIO_GPU_UPDATE_DESC_COUNT;
            if (slot < VIRTIO_GPU_UPDATE_SLOT_COUNT && virtio_gpu_update_slots[slot].active) {
                virtio_gpu_update_slots[slot].completions++;
                if (virtio_gpu_update_slots[slot].completions == 2) virtio_gpu_update_slots[slot].active = false;
            }
        } else {
            break;
        }
        virtio_gpu_control_queue.last_used_idx++;
    }
}

static int wait_for_virtio_gpu_updates(void) {
    uint64_t start = get_monotonic_time_us();
    for (;;) {
        reap_virtio_gpu_updates();
        bool active = false;
        for (uint32_t i = 0; i < VIRTIO_GPU_UPDATE_SLOT_COUNT; i++) active |= virtio_gpu_update_slots[i].active;
        if (!active) return 0;
        if (get_monotonic_time_us() - start >= VIRTIO_GPU_QUEUE_TIMEOUT_US) return -ETIMEDOUT;
        __asm__ volatile ("pause");
    }
}

static int submit_virtio_gpu_command(const void *request, uint32_t request_size, uint32_t expected_response) {
    if (!virtio_gpu_common || !virtio_gpu_control_queue.notify || !request || !request_size || request_size > PAGE_SIZE) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&virtio_gpu_lock, &irq);
    int result = wait_for_virtio_gpu_updates();
    if (result < 0) { spin_unlock_irqrestore(&virtio_gpu_lock, irq); return result; }
    memcpy(virtio_gpu_command, request, request_size);
    memset(virtio_gpu_response, 0, PAGE_SIZE);
    virtio_gpu_control_queue.desc[0] = (virtq_desc_t){
        .addr = virtio_gpu_command_phys,
        .len = request_size,
        .flags = VIRTQ_DESC_F_NEXT,
        .next = 1,
    };
    virtio_gpu_control_queue.desc[1] = (virtq_desc_t){
        .addr = virtio_gpu_response_phys,
        .len = PAGE_SIZE,
        .flags = VIRTQ_DESC_F_WRITE,
        .next = 0,
    };
    uint16_t avail_idx = virtio_gpu_control_queue.avail->idx;
    virtio_gpu_control_queue.avail->ring[avail_idx % virtio_gpu_control_queue.size] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    virtio_gpu_control_queue.avail->idx = avail_idx + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *virtio_gpu_control_queue.notify = VIRTIO_GPU_CONTROL_QUEUE;

    uint64_t start = get_monotonic_time_us();
    while (virtio_gpu_control_queue.used->idx == virtio_gpu_control_queue.last_used_idx) {
        if (get_monotonic_time_us() - start >= VIRTIO_GPU_QUEUE_TIMEOUT_US) {
            spin_unlock_irqrestore(&virtio_gpu_lock, irq);
            return -ETIMEDOUT;
        }
        __asm__ volatile ("pause");
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16_t used_slot = virtio_gpu_control_queue.last_used_idx % virtio_gpu_control_queue.size;
    uint32_t used_descriptor = virtio_gpu_control_queue.used->ring[used_slot].id;
    virtio_gpu_control_queue.last_used_idx++;
    uint32_t used_len = virtio_gpu_control_queue.used->ring[used_slot].len;
    uint32_t response = ((virtio_gpu_ctrl_hdr_t *)virtio_gpu_response)->type;
    result = used_descriptor == 0 && used_len >= sizeof(virtio_gpu_ctrl_hdr_t) && response == expected_response ? 0 : -EIO;
    spin_unlock_irqrestore(&virtio_gpu_lock, irq);
    return result;
}

static int unref_virtio_gpu_resource(uint32_t resource_id) {
    if (!resource_id) return 0;
    virtio_gpu_resource_unref_t unref = {
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF,
        .resource_id = resource_id,
    };
    return submit_virtio_gpu_command(&unref, sizeof(unref), VIRTIO_GPU_RESP_OK_NODATA);
}

static int create_virtio_gpu_resource(uint32_t resource_id, uint64_t framebuffer_phys, uint32_t framebuffer_size, uint32_t width, uint32_t height) {
    virtio_gpu_resource_create_2d_t create = {
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
        .resource_id = resource_id,
        .format = VIRTIO_GPU_FRAMEBUFFER_FORMAT,
        .width = width,
        .height = height,
    };
    int result = submit_virtio_gpu_command(&create, sizeof(create), VIRTIO_GPU_RESP_OK_NODATA);
    if (result < 0) return result;

    virtio_gpu_resource_attach_backing_command_t attach = {
        .request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
        .request.resource_id = resource_id,
        .request.nr_entries = 1,
        .entry.addr = framebuffer_phys,
        .entry.length = framebuffer_size,
    };
    result = submit_virtio_gpu_command(&attach, sizeof(attach), VIRTIO_GPU_RESP_OK_NODATA);
    if (result < 0) return result;

    virtio_gpu_set_scanout_t scanout = {
        .hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .rect = {.width = width, .height = height},
        .scanout_id = virtio_gpu_scanout,
        .resource_id = resource_id,
    };
    return submit_virtio_gpu_command(&scanout, sizeof(scanout), VIRTIO_GPU_RESP_OK_NODATA);
}

int update_virtio_gpu(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    if (!virtio_gpu_ready) return -ENODEV;
    if (!width || !height) return 0;
    if (x > virtio_gpu_width || y > virtio_gpu_height || width > virtio_gpu_width - x || height > virtio_gpu_height - y) return -EINVAL;
    if (x > UINT32_MAX || y > UINT32_MAX || width > UINT32_MAX || height > UINT32_MAX) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&virtio_gpu_lock, &irq);
    uint64_t start = get_monotonic_time_us();
    uint32_t slot;
    for (;;) {
        reap_virtio_gpu_updates();
        for (slot = 0; slot < VIRTIO_GPU_UPDATE_SLOT_COUNT; slot++) {
            if (!virtio_gpu_update_slots[slot].active) break;
        }
        if (slot < VIRTIO_GPU_UPDATE_SLOT_COUNT) break;
        if (get_monotonic_time_us() - start >= VIRTIO_GPU_QUEUE_TIMEOUT_US) {
            spin_unlock_irqrestore(&virtio_gpu_lock, irq);
            return -ETIMEDOUT;
        }
        __asm__ volatile ("pause");
    }

    uint64_t request_offset = (uint64_t)slot * 2 * VIRTIO_GPU_UPDATE_REQUEST_STRIDE;
    uint64_t response_offset = (uint64_t)slot * 2 * VIRTIO_GPU_UPDATE_RESPONSE_STRIDE;
    virtio_gpu_transfer_to_host_2d_t *transfer = (void *)((uint8_t *)virtio_gpu_update_commands + request_offset);
    virtio_gpu_resource_flush_t *flush = (void *)((uint8_t *)virtio_gpu_update_commands + request_offset + VIRTIO_GPU_UPDATE_REQUEST_STRIDE);
    memset(transfer, 0, VIRTIO_GPU_UPDATE_REQUEST_STRIDE);
    memset(flush, 0, VIRTIO_GPU_UPDATE_REQUEST_STRIDE);
    *transfer = (virtio_gpu_transfer_to_host_2d_t){
        .hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
        .rect = {.x = x, .y = y, .width = width, .height = height},
        .offset = (y * virtio_gpu_width + x) * sizeof(uint32_t),
        .resource_id = virtio_gpu_resource_id,
    };
    *flush = (virtio_gpu_resource_flush_t){
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .rect = {.x = x, .y = y, .width = width, .height = height},
        .resource_id = virtio_gpu_resource_id,
    };
    memset((uint8_t *)virtio_gpu_update_responses + response_offset, 0, 2 * VIRTIO_GPU_UPDATE_RESPONSE_STRIDE);

    uint16_t descriptor = VIRTIO_GPU_SYNC_DESC_COUNT + slot * VIRTIO_GPU_UPDATE_DESC_COUNT;
    virtio_gpu_control_queue.desc[descriptor] = (virtq_desc_t){
        .addr = virtio_gpu_update_commands_phys + request_offset,
        .len = sizeof(*transfer),
        .flags = VIRTQ_DESC_F_NEXT,
        .next = descriptor + 1,
    };
    virtio_gpu_control_queue.desc[descriptor + 1] = (virtq_desc_t){
        .addr = virtio_gpu_update_responses_phys + response_offset,
        .len = VIRTIO_GPU_UPDATE_RESPONSE_STRIDE,
        .flags = VIRTQ_DESC_F_WRITE,
    };
    virtio_gpu_control_queue.desc[descriptor + 2] = (virtq_desc_t){
        .addr = virtio_gpu_update_commands_phys + request_offset + VIRTIO_GPU_UPDATE_REQUEST_STRIDE,
        .len = sizeof(*flush),
        .flags = VIRTQ_DESC_F_NEXT,
        .next = descriptor + 3,
    };
    virtio_gpu_control_queue.desc[descriptor + 3] = (virtq_desc_t){
        .addr = virtio_gpu_update_responses_phys + response_offset + VIRTIO_GPU_UPDATE_RESPONSE_STRIDE,
        .len = VIRTIO_GPU_UPDATE_RESPONSE_STRIDE,
        .flags = VIRTQ_DESC_F_WRITE,
    };
    virtio_gpu_update_slots[slot].completions = 0;
    virtio_gpu_update_slots[slot].active = true;
    uint16_t avail_idx = virtio_gpu_control_queue.avail->idx;
    virtio_gpu_control_queue.avail->ring[avail_idx % virtio_gpu_control_queue.size] = descriptor;
    virtio_gpu_control_queue.avail->ring[(avail_idx + 1) % virtio_gpu_control_queue.size] = descriptor + 2;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    virtio_gpu_control_queue.avail->idx = avail_idx + 2;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *virtio_gpu_control_queue.notify = VIRTIO_GPU_CONTROL_QUEUE;
    spin_unlock_irqrestore(&virtio_gpu_lock, irq);
    return 0;
}

int set_virtio_gpu_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp) {
    if (!virtio_gpu_transport_ready) return -ENODEV;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return -ENODEV;
    if (xres_virtual != xres || yres_virtual != yres || xoffset || yoffset) return -EOPNOTSUPP;
    if (bpp != VIRTIO_GPU_BPP || !xres || !yres || xres > UINT32_MAX || yres > UINT32_MAX) return -EINVAL;
    if (xres > UINT32_MAX / VIRTIO_GPU_BYTES_PER_PIXEL || yres > UINT32_MAX / (xres * VIRTIO_GPU_BYTES_PER_PIXEL)) return -EOVERFLOW;

    uint64_t framebuffer_size = xres * yres * VIRTIO_GPU_BYTES_PER_PIXEL;
    uint64_t allocation_size = (framebuffer_size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    void *framebuffer = prealloc(allocation_size / PAGE_SIZE);
    if (!framebuffer) return -ENOMEM;
    uint64_t framebuffer_phys = (uint64_t)framebuffer;
    memset(phys_to_virt(framebuffer_phys), 0, framebuffer_size);

    uint32_t resource_id = virtio_gpu_next_resource_id++;
    if (!resource_id) resource_id = virtio_gpu_next_resource_id++;
    int result = create_virtio_gpu_resource(resource_id, framebuffer_phys, (uint32_t)framebuffer_size, (uint32_t)xres, (uint32_t)yres);
    if (result < 0) {
        (void)unref_virtio_gpu_resource(resource_id);
        pfree_range(framebuffer, allocation_size);
        return result;
    }

    uint32_t old_resource_id = virtio_gpu_resource_id;
    uint64_t old_framebuffer_phys = virtio_gpu_framebuffer_phys;
    uint64_t old_allocation_size = virtio_gpu_framebuffer_allocation_size;
    virtio_gpu_resource_id = resource_id;
    virtio_gpu_framebuffer_phys = framebuffer_phys;
    virtio_gpu_framebuffer_allocation_size = allocation_size;
    virtio_gpu_width = (uint32_t)xres;
    virtio_gpu_height = (uint32_t)yres;

    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    fb->address = phys_to_virt(framebuffer_phys);
    fb->width = xres;
    fb->height = yres;
    fb->pitch = xres * VIRTIO_GPU_BYTES_PER_PIXEL;
    fb->bpp = VIRTIO_GPU_BPP;
    fb->memory_model = LIMINE_FRAMEBUFFER_RGB;
    fb->red_mask_size = 8;
    fb->red_mask_shift = 16;
    fb->green_mask_size = 8;
    fb->green_mask_shift = 8;
    fb->blue_mask_size = 8;
    fb->blue_mask_shift = 0;
    fb_xres_virtual = xres;
    fb_yres_virtual = yres;
    fb_xoffset = 0;
    fb_yoffset = 0;
    virtio_gpu_ready = true;
    current_fb_driver = FB_VIRTIO_GPU;
    sync_terminal();

    if (old_resource_id) (void)unref_virtio_gpu_resource(old_resource_id);
    if (old_framebuffer_phys && old_allocation_size) pfree_range((void *)old_framebuffer_phys, old_allocation_size);
    return 0;
}

void init_virtio_gpu(pci_device_t *dev) {
    if (!dev || virtio_gpu_ready) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    set_pci_d0(dev);
    uint32_t command = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, command | 0x6);
    if (!find_virtio_capabilities(dev)) { log("virtio-gpu: modern pci capabilities are unavailable\n"); return; }
    if (!negotiate_virtio_gpu_features()) { set_virtio_gpu_status(VIRTIO_STATUS_FAILED); log("virtio-gpu: feature negotiation failed\n"); return; }
    if (!initialize_virtio_gpu_control_queue()) { set_virtio_gpu_status(VIRTIO_STATUS_FAILED); log("virtio-gpu: unable to initialize control queue\n"); return; }
    if (!allocate_virtio_gpu_queue_page(&virtio_gpu_command_phys, &virtio_gpu_command)
        || !allocate_virtio_gpu_queue_page(&virtio_gpu_response_phys, &virtio_gpu_response)
        || !allocate_virtio_gpu_queue_page(&virtio_gpu_update_commands_phys, &virtio_gpu_update_commands)
        || !allocate_virtio_gpu_queue_page(&virtio_gpu_update_responses_phys, &virtio_gpu_update_responses)) {
        set_virtio_gpu_status(VIRTIO_STATUS_FAILED);
        log("virtio-gpu: unable to allocate command buffers\n");
        return;
    }
    set_virtio_gpu_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    virtio_gpu_transport_ready = true;

    virtio_gpu_ctrl_hdr_t display_request = {.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO};
    if (submit_virtio_gpu_command(&display_request, sizeof(display_request), VIRTIO_GPU_RESP_OK_DISPLAY_INFO) < 0) {
        set_virtio_gpu_status(VIRTIO_STATUS_FAILED);
        virtio_gpu_transport_ready = false;
        log("virtio-gpu: unable to query displays\n");
        return;
    }
    virtio_gpu_resp_display_info_t display_info;
    memcpy(&display_info, virtio_gpu_response, sizeof(display_info));
    bool found = false;
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (display_info.pmodes[i].enabled != VIRTIO_GPU_DISPLAY_INFO_ENABLED || !display_info.pmodes[i].rect.width || !display_info.pmodes[i].rect.height) continue;
        virtio_gpu_scanout = i;
        virtio_gpu_width = display_info.pmodes[i].rect.width;
        virtio_gpu_height = display_info.pmodes[i].rect.height;
        found = true;
        break;
    }
    if (!found) {
        set_virtio_gpu_status(VIRTIO_STATUS_FAILED);
        virtio_gpu_transport_ready = false;
        log("virtio-gpu: no usable scanout found\n");
        return;
    }
    if (set_virtio_gpu_resolution(virtio_gpu_width, virtio_gpu_height, virtio_gpu_width, virtio_gpu_height, 0, 0, VIRTIO_GPU_BPP) < 0) {
        set_virtio_gpu_status(VIRTIO_STATUS_FAILED);
        virtio_gpu_transport_ready = false;
        log("virtio-gpu: unable to set initial resolution\n");
        return;
    }
    log("virtio-gpu: initialized virtio-gpu\n");
}
