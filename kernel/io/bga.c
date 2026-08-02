#include <freestanding/stdbool.h>
#include <freestanding/stdint.h>
#include <main/limine_req.h>
#include <io/io.h>
#include <io/pci.h>
#include <io/terminal.h>
#include <io/bga.h>
#include <mm/vmm.h>

static uint16_t bga_version = 0;

static uint16_t read_bga_register(uint16_t index) {
    outw(BGA_IOPORT_INDEX, index);
    return inw(BGA_IOPORT_DATA);
}

static void write_bga_register(uint16_t index, uint16_t value) {
    outw(BGA_IOPORT_INDEX, index);
    outw(BGA_IOPORT_DATA, value);
}

static bool bpp_supported_by_bga(uint16_t bpp) {
    switch (bpp) {
        case 8:
            return true;
        case 15:
        case 16:
        case 24:
        case 32:
            return bga_version >= BGA_DISPI_ID2;
        default:
            return false;
    }
}

uint8_t bga_palette_index(uint32_t color) {
    uint8_t red = (color >> 16) & 0xFF;
    uint8_t green = (color >> 8) & 0xFF;
    uint8_t blue = color & 0xFF;

    return (uint8_t)((red & 0xE0) | ((green & 0xE0) >> 3) | (blue >> 6));
}

uint32_t bga_palette_color(uint8_t index) {
    uint32_t red = ((index >> 5) & 0x07) * 255 / 7;
    uint32_t green = ((index >> 2) & 0x07) * 255 / 7;
    uint32_t blue = (index & 0x03) * 255 / 3;

    return (red << 16) | (green << 8) | blue;
}

static void program_bga_palette(bool eight_bit_dac) {
    outb(BGA_DAC_WRITE_INDEX, 0);

    for (uint16_t i = 0; i < 256; i++) {
        uint32_t color = bga_palette_color((uint8_t)i);
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

void set_bga_fb(uint64_t width, uint64_t height, uint16_t bpp) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    if (bga_version == 0) {
        printf("bga: adapter is not initialized\n");
        return;
    }

    if (width < 320 || width > 2560 || height < 200 || height > 1600) {
        printf("bga: unsupported resolution\n");
        return;
    }

    if (!bpp_supported_by_bga(bpp)) {
        printf("bga: unsupported bpp\n");
        return;
    }

    write_bga_register(BGA_INDEX_ENABLE, BGA_DISABLED);
    write_bga_register(BGA_INDEX_XRES, width);
    write_bga_register(BGA_INDEX_YRES, height);
    write_bga_register(BGA_INDEX_BPP, bpp);
    uint16_t enable_flags = BGA_ENABLED | BGA_LFB_ENABLED;
    bool eight_bit_dac = bpp == 8 && bga_version >= BGA_DISPI_ID4;
    if (eight_bit_dac) enable_flags |= BGA_8BIT_DAC;
    write_bga_register(BGA_INDEX_ENABLE, enable_flags);

    if (bpp == 8) program_bga_palette(eight_bit_dac);

    if (bga_version >= BGA_DISPI_ID1) write_bga_register(BGA_INDEX_VIRT_WIDTH, width);

    uint16_t actual_width = read_bga_register(BGA_INDEX_XRES);
    uint16_t actual_height = read_bga_register(BGA_INDEX_YRES);
    uint16_t actual_bpp = read_bga_register(BGA_INDEX_BPP);
    uint16_t virtual_width = actual_width;

    if (bga_version >= BGA_DISPI_ID1) virtual_width = read_bga_register(BGA_INDEX_VIRT_WIDTH);

    if (actual_width != width || actual_height != height || actual_bpp != bpp || virtual_width < actual_width) {
        printf("bga: adapter rejected framebuffer mode\n");
        return;
    }

    fb->width = (uint64_t)actual_width;
    fb->height = (uint64_t)actual_height;
    fb->pitch = (uint64_t)virtual_width * ((actual_bpp + 7) / 8);
    fb->bpp = actual_bpp;

    update_framebuffer_masks();
    sync_terminal();
}

void init_bga(pci_device_t *dev) {
    if (!dev) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    set_pci_d0(dev);

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);

    if (bar0 & 0x1) {
        printf("bga: bar0 is not a memory bar\n");
        return;
    }

    uint32_t bar_type = (bar0 >> 1) & 0x3;
    if (bar_type == 0x2) {
        printf("bga: 64-bit bar0 is unsupported\n");
        return;
    }

    if (bar_type == 0x3) {
        printf("bga: invalid bar0 type\n");
        return;
    }

    uint32_t phys_vram = bar0 & 0xFFFFFFF0;
    if (phys_vram == 0) {
        printf("bga: invalid bar0 vram address\n");
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
            printf("bga: invalid version id\n");
            return;
    }

    uint64_t width = fb->width;
    uint64_t height = fb->height;
    uint16_t bpp = fb->bpp;

    if (width < 320 || width > 2560 || height < 200 || height > 1600) {
        printf("bga: unsupported resolution\n");
        return;
    }

    if (!bpp_supported_by_bga(bpp)) {
        printf("bga: unsupported bpp\n");
        return;
    }

    fb->address = phys_to_virt(phys_vram);
    set_bga_fb((uint16_t)width, (uint16_t)height, bpp);

    if (read_bga_register(BGA_INDEX_XRES) != width || read_bga_register(BGA_INDEX_YRES) != height || read_bga_register(BGA_INDEX_BPP) != bpp) return;

    printf("bga: initialized bga\n");
}
