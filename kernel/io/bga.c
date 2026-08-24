#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/limine_req.h>
#include <io/io.h>
#include <io/fb.h>
#include <io/pci.h>
#include <io/terminal.h>
#include <io/bga.h>
#include <mm/vmm.h>

static uint16_t bga_version = 0;
static uint32_t bga_vram_size = 0;
static bool bga_ready = false;

static uint16_t read_bga_register(uint16_t index) {
    outw(BGA_IOPORT_INDEX, index);
    return inw(BGA_IOPORT_DATA);
}

static void write_bga_register(uint16_t index, uint16_t value) {
    outw(BGA_IOPORT_INDEX, index);
    outw(BGA_IOPORT_DATA, value);
}

static uint32_t get_bga_vram_size(pci_device_t *dev, uint32_t bar0) {
    uint32_t command = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, command & ~0x2u);
    write_pci(dev->bus, dev->dev, dev->func, 0x10, 0xFFFFFFFFu);
    uint32_t mask = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    write_pci(dev->bus, dev->dev, dev->func, 0x10, bar0);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, command);
    mask &= 0xFFFFFFF0u;
    return mask ? (~mask + 1u) : 0;
}

static bool is_bpp_supported_by_bga(uint16_t bpp) {
    switch (bpp) {
        case 15:
        case 16:
        case 24:
        case 32:
            return bga_version >= BGA_DISPI_ID2;
        default:
            return false;
    }
}

static void program_bga_palette(bool eight_bit_dac) {
    outb(BGA_DAC_WRITE_INDEX, 0);

    for (uint16_t i = 0; i < 256; i++) {
        uint32_t color = palette_color_for_bga((uint8_t)i);
        uint8_t red = (color >> 16) & 0xFF;
        uint8_t green = (color >> 8) & 0xFF;
        uint8_t blue = color & 0xFF;

        if (!eight_bit_dac) {
            red >>= 2;
            green >>= 2;
            blue >>= 2;
        }

        outb(BGA_DAC_DATA, red);
        outb(BGA_DAC_DATA, green);
        outb(BGA_DAC_DATA, blue);
    }
}

static void update_framebuffer_masks(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    switch (fb->bpp) {
        case 8:
            fb->red_mask_size = 0;
            fb->red_mask_shift = 0;
            fb->green_mask_size = 0;
            fb->green_mask_shift = 0;
            fb->blue_mask_size = 0;
            fb->blue_mask_shift = 0;
            break;
        case 15:
            fb->red_mask_size = 5;
            fb->red_mask_shift = 10;
            fb->green_mask_size = 5;
            fb->green_mask_shift = 5;
            fb->blue_mask_size = 5;
            fb->blue_mask_shift = 0;
            break;
        case 16:
            fb->red_mask_size = 5;
            fb->red_mask_shift = 11;
            fb->green_mask_size = 6;
            fb->green_mask_shift = 5;
            fb->blue_mask_size = 5;
            fb->blue_mask_shift = 0;
            break;
        case 24:
        case 32:
            fb->red_mask_size = 8;
            fb->red_mask_shift = 16;
            fb->green_mask_size = 8;
            fb->green_mask_shift = 8;
            fb->blue_mask_size = 8;
            fb->blue_mask_shift = 0;
            break;
        default:
            return;
    }
}

uint32_t palette_color_for_bga(uint8_t index) {
    uint32_t red = ((index >> 5) & 0x07) * 255 / 7;
    uint32_t green = ((index >> 2) & 0x07) * 255 / 7;
    uint32_t blue = (index & 0x03) * 255 / 3;

    return (red << 16) | (green << 8) | blue;
}

int set_bga_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return -ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    if (!bga_ready || bga_version == 0) {
        log("bga: adapter is not initialized\n");
        return -ENODEV;
    }

    if (xres < 320 || xres > 2560 || yres < 200 || yres > 1600) {
        log("bga: unsupported resolution\n");
        return -EINVAL;
    }

    if (xres_virtual < xres || yres_virtual < yres || xres_virtual > UINT16_MAX || yres_virtual > UINT16_MAX || xoffset > xres_virtual - xres || yoffset > yres_virtual - yres) {
        log("bga: invalid virtual resolution or offset\n");
        return -EINVAL;
    }

    if (bpp == 8) {
        log("bga: bpp not allowed\n");
        return -EOPNOTSUPP;
    }

    if (!is_bpp_supported_by_bga(bpp)) {
        log("bga: unsupported bpp\n");
        return -EINVAL;
    }

    uint64_t bytes_per_pixel = (bpp + 7) / 8;
    if (xres_virtual > UINT64_MAX / yres_virtual || xres_virtual * yres_virtual > UINT64_MAX / bytes_per_pixel || xres_virtual * yres_virtual * bytes_per_pixel > bga_vram_size) {
        log("bga: mode exceeds vram size\n");
        return -ENOMEM;
    }

    uint16_t old_xres = read_bga_register(BGA_INDEX_XRES);
    uint16_t old_yres = read_bga_register(BGA_INDEX_YRES);
    uint16_t old_bpp = read_bga_register(BGA_INDEX_BPP);
    uint16_t old_xres_virtual = read_bga_register(BGA_INDEX_VIRT_WIDTH);
    uint16_t old_x_offset = read_bga_register(BGA_INDEX_X_OFFSET);
    uint16_t old_y_offset = read_bga_register(BGA_INDEX_Y_OFFSET);
    uint16_t old_enable = read_bga_register(BGA_INDEX_ENABLE);

    write_bga_register(BGA_INDEX_ENABLE, BGA_DISABLED);
    write_bga_register(BGA_INDEX_XRES, xres);
    write_bga_register(BGA_INDEX_YRES, yres);
    write_bga_register(BGA_INDEX_BPP, bpp);
    uint16_t enable_flags = BGA_ENABLED | BGA_LFB_ENABLED;
    bool eight_bit_dac = bpp == 8 && bga_version >= BGA_DISPI_ID4;
    if (eight_bit_dac) enable_flags |= BGA_8BIT_DAC;
    write_bga_register(BGA_INDEX_ENABLE, enable_flags);

    if (bpp == 8) program_bga_palette(eight_bit_dac);
    if (bga_version >= BGA_DISPI_ID1) write_bga_register(BGA_INDEX_VIRT_WIDTH, xres_virtual);
    write_bga_register(BGA_INDEX_X_OFFSET, xoffset);
    write_bga_register(BGA_INDEX_Y_OFFSET, yoffset);

    uint16_t actual_xres = read_bga_register(BGA_INDEX_XRES);
    uint16_t actual_yres = read_bga_register(BGA_INDEX_YRES);
    uint16_t actual_bpp = read_bga_register(BGA_INDEX_BPP);
    uint16_t actual_xres_virtual = actual_xres;
    uint16_t maximum_yres_virtual = actual_yres;

    if (bga_version >= BGA_DISPI_ID1) {
        actual_xres_virtual = read_bga_register(BGA_INDEX_VIRT_WIDTH);
        maximum_yres_virtual = read_bga_register(BGA_INDEX_VIRT_HEIGHT);
    }

    if (actual_xres != xres || actual_yres != yres || actual_bpp != bpp || actual_xres_virtual < xres || maximum_yres_virtual < yres_virtual || read_bga_register(BGA_INDEX_X_OFFSET) != xoffset || read_bga_register(BGA_INDEX_Y_OFFSET) != yoffset) {
        write_bga_register(BGA_INDEX_ENABLE, BGA_DISABLED);
        write_bga_register(BGA_INDEX_XRES, old_xres);
        write_bga_register(BGA_INDEX_YRES, old_yres);
        write_bga_register(BGA_INDEX_BPP, old_bpp);
        if (bga_version >= BGA_DISPI_ID1) write_bga_register(BGA_INDEX_VIRT_WIDTH, old_xres_virtual);
        write_bga_register(BGA_INDEX_X_OFFSET, old_x_offset);
        write_bga_register(BGA_INDEX_Y_OFFSET, old_y_offset);
        write_bga_register(BGA_INDEX_ENABLE, old_enable);
        log("bga: adapter rejected framebuffer mode\n");
        return -EINVAL;
    }

    fb->width = (uint64_t)actual_xres;
    fb->height = (uint64_t)actual_yres;
    fb->pitch = (uint64_t)actual_xres_virtual * ((actual_bpp + 7) / 8);
    fb->bpp = actual_bpp;
    fb_xres_virtual = actual_xres_virtual;
    fb_yres_virtual = yres_virtual;
    fb_xoffset = xoffset;
    fb_yoffset = yoffset;

    update_framebuffer_masks();
    sync_terminal();
    return 0;
}

void init_bga(pci_device_t *dev) {
    if (!dev) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    set_pci_d0(dev);

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);

    if (bar0 & 0x1) {
        log("bga: bar0 is not a memory bar\n");
        return;
    }

    uint32_t bar_type = (bar0 >> 1) & 0x3;
    if (bar_type == 0x2) {
        log("bga: 64-bit bar0 is unsupported\n");
        return;
    }

    if (bar_type == 0x3) {
        log("bga: invalid bar0 type\n");
        return;
    }

    uint32_t phys_vram = bar0 & 0xFFFFFFF0;
    if (phys_vram == 0) {
        log("bga: invalid bar0 vram address\n");
        return;
    }

    bga_vram_size = get_bga_vram_size(dev, bar0);
    if (bga_vram_size < PAGE_SIZE) {
        log("bga: invalid vram bar size\n");
        return;
    }

    write_bga_register(BGA_INDEX_ID, BGA_DISPI_ID5);
    bga_version = read_bga_register(BGA_INDEX_ID);

    switch (bga_version) {
        case BGA_DISPI_ID0:
        case BGA_DISPI_ID1:
        case BGA_DISPI_ID2:
        case BGA_DISPI_ID3:
        case BGA_DISPI_ID4:
        case BGA_DISPI_ID5:
            break;
        default:
            bga_version = 0;
            log("bga: invalid version id\n");
            return;
    }

    uint64_t xres = fb->width;
    uint64_t yres = fb->height;
    uint16_t bpp = fb->bpp;

    if (xres < 320 || xres > 2560 || yres < 200 || yres > 1600) {
        log("bga: unsupported resolution\n");
        return;
    }

    if (!is_bpp_supported_by_bga(bpp)) {
        log("bga: unsupported bpp\n");
        return;
    }

    fb->address = vmap_mmio(phys_vram, (bga_vram_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!fb->address) {
        log("bga: unable to map vram\n");
        return;
    }

    bga_ready = true;

    if (set_bga_resolution(xres, yres, xres, yres, 0, 0, bpp) < 0) {
        bga_ready = false;
        return;
    }

    if (read_bga_register(BGA_INDEX_XRES) != xres || read_bga_register(BGA_INDEX_YRES) != yres || read_bga_register(BGA_INDEX_BPP) != bpp) return;

    current_fb_driver = FB_BGA;
    log("bga: initialized bga\n");
}
