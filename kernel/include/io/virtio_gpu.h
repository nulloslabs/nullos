#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <io/pci.h>

#define VIRTIO_GPU_VENDOR 0x1AF4
#define VIRTIO_GPU_DEVICE_MODERN 0x1050
#define VIRTIO_GPU_DEVICE_TRANSITIONAL 0x1010

#define VIRTIO_PCI_CAP_ID 0x09
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_FAILED 0x80

#define VIRTIO_F_VERSION_1 32

#define VIRTIO_GPU_CONTROL_QUEUE 0
#define VIRTIO_GPU_QUEUE_SIZE 64
#define VIRTIO_GPU_SYNC_DESC_COUNT 2
#define VIRTIO_GPU_UPDATE_DESC_COUNT 4
#define VIRTIO_GPU_UPDATE_SLOT_COUNT ((VIRTIO_GPU_QUEUE_SIZE - VIRTIO_GPU_SYNC_DESC_COUNT) / VIRTIO_GPU_UPDATE_DESC_COUNT)
#define VIRTIO_GPU_UPDATE_REQUEST_STRIDE 64
#define VIRTIO_GPU_UPDATE_RESPONSE_STRIDE 32
#define VIRTIO_GPU_QUEUE_TIMEOUT_US 1000000ULL
#define VIRTIO_GPU_FIRST_RESOURCE_ID 1
#define VIRTIO_GPU_MAX_SCANOUTS 16
#define VIRTIO_GPU_BPP 32
#define VIRTIO_GPU_BYTES_PER_PIXEL 4

#define VIRTQ_DESC_F_NEXT 0x01
#define VIRTQ_DESC_F_WRITE 0x02
#define VIRTQ_AVAIL_F_NO_INTERRUPT 0x01

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FRAMEBUFFER_FORMAT VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM
#define VIRTIO_GPU_DISPLAY_INFO_ENABLED 1

typedef struct __attribute__((packed)) {
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t id;
    uint8_t padding[2];
    uint32_t offset;
    uint32_t length;
} virtio_pci_cap_t;

typedef struct __attribute__((packed)) {
    virtio_pci_cap_t cap;
    uint32_t notify_off_multiplier;
} virtio_pci_notify_cap_t;

typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} virtio_pci_common_cfg_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} virtio_gpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_rect_t rect;
    uint32_t enabled;
    uint32_t flags;
} virtio_gpu_display_one_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_unref_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_flush_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} virtio_gpu_resource_attach_backing_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_mem_entry_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_resource_attach_backing_t request;
    virtio_gpu_mem_entry_t entry;
} virtio_gpu_resource_attach_backing_command_t;

typedef struct {
    volatile virtq_desc_t *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t *used;
    volatile uint16_t *notify;
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;
    uint16_t size;
    uint16_t last_used_idx;
} virtio_gpu_queue_t;

typedef struct {
    uint8_t completions;
    bool active;
} virtio_gpu_update_slot_t;

int update_virtio_gpu(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
int set_virtio_gpu_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp);
void init_virtio_gpu(pci_device_t *dev);
