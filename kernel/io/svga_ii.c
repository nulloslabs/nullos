#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <io/io.h>
#include <io/fb.h>
#include <io/pci.h>
#include <io/svga_ii.h>
#include <io/terminal.h>
#include <mm/vmm.h>

static uint16_t svga_ii_io_base = 0;
static uint8_t *svga_ii_vram = NULL;
static volatile uint32_t *svga_ii_fifo = NULL;
static uint32_t svga_ii_vram_size = 0;
static uint32_t svga_ii_fifo_size = 0;
static bool svga_ii_ready = false;
static spinlock_t svga_ii_fifo_lock = SPINLOCK_INIT;

static uint32_t read_svga_ii_register(uint32_t index) {
    outl(svga_ii_io_base + SVGA_II_INDEX_PORT, index);
    return inl(svga_ii_io_base + SVGA_II_VALUE_PORT);
}

static void write_svga_ii_register(uint32_t index, uint32_t value) {
    outl(svga_ii_io_base + SVGA_II_INDEX_PORT, index);
    outl(svga_ii_io_base + SVGA_II_VALUE_PORT, value);
}

static int sync_svga_ii(void) {
    write_svga_ii_register(SVGA_II_REG_SYNC, 1);
    for (uint32_t i = 0; i < SVGA_II_SYNC_LIMIT; i++) {
        if (!read_svga_ii_register(SVGA_II_REG_BUSY)) return 0;
        __asm__ volatile ("pause");
    }
    return -EIO;
}

static bool get_svga_ii_memory_bar(pci_device_t *dev, uint8_t offset, uint32_t *address) {
    uint32_t bar = read_pci(dev->bus, dev->dev, dev->func, offset);
    if (bar & 0x1) return false;
    if (((bar >> 1) & 0x3) != 0) return false;
    *address = bar & 0xFFFFFFF0u;
    return *address != 0;
}

static void set_svga_ii_mask(uint32_t mask, uint8_t *size, uint8_t *shift) {
    *size = 0;
    *shift = 0;
    if (!mask) return;
    while (!(mask & 1)) {
        (*shift)++;
        mask >>= 1;
    }
    while (mask & 1) {
        (*size)++;
        mask >>= 1;
    }
}

static bool initialize_svga_ii_fifo(void) {
    uint32_t registers = read_svga_ii_register(SVGA_II_REG_MEM_REGS);
    if (registers < SVGA_II_FIFO_REGS) registers = SVGA_II_FIFO_REGS;
    if (registers > UINT32_MAX / sizeof(uint32_t)) return false;
    uint32_t minimum = registers * sizeof(uint32_t);
    if (minimum > svga_ii_fifo_size || svga_ii_fifo_size - minimum <= 5 * sizeof(uint32_t)) return false;

    write_svga_ii_register(SVGA_II_REG_CONFIG_DONE, 0);
    svga_ii_fifo[SVGA_II_FIFO_MIN] = minimum;
    svga_ii_fifo[SVGA_II_FIFO_MAX] = svga_ii_fifo_size;
    svga_ii_fifo[SVGA_II_FIFO_NEXT_CMD] = minimum;
    svga_ii_fifo[SVGA_II_FIFO_STOP] = minimum;
    __asm__ volatile ("" ::: "memory");
    write_svga_ii_register(SVGA_II_REG_CONFIG_DONE, 1);
    return true;
}

static void write_svga_ii_fifo(uint32_t *offset, uint32_t value) {
    svga_ii_fifo[*offset / sizeof(uint32_t)] = value;
    *offset += sizeof(uint32_t);
    if (*offset == svga_ii_fifo[SVGA_II_FIFO_MAX]) *offset = svga_ii_fifo[SVGA_II_FIFO_MIN];
}

static uint32_t get_svga_ii_fifo_free(uint32_t next, uint32_t stop, uint32_t minimum, uint32_t maximum) {
    if (next >= stop) return maximum - next + stop - minimum - sizeof(uint32_t);
    return stop - next - sizeof(uint32_t);
}

int update_svga_ii(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    if (!svga_ii_ready || !svga_ii_fifo) return -ENODEV;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return -ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    if (!width || !height) return 0;
    if (x > fb->width || y > fb->height || width > fb->width - x || height > fb->height - y) return -EINVAL;
    if (x > UINT32_MAX || y > UINT32_MAX || width > UINT32_MAX || height > UINT32_MAX) return -EINVAL;

    uint64_t rflags;
    spin_lock_irqsave(&svga_ii_fifo_lock, &rflags);
    uint32_t next = svga_ii_fifo[SVGA_II_FIFO_NEXT_CMD];
    uint32_t stop = svga_ii_fifo[SVGA_II_FIFO_STOP];
    uint32_t minimum = svga_ii_fifo[SVGA_II_FIFO_MIN];
    uint32_t maximum = svga_ii_fifo[SVGA_II_FIFO_MAX];
    if (minimum >= maximum || next < minimum || next >= maximum || stop < minimum || stop >= maximum) {
        spin_unlock_irqrestore(&svga_ii_fifo_lock, rflags);
        return -EIO;
    }

    uint32_t command_size = 5 * sizeof(uint32_t);
    if (get_svga_ii_fifo_free(next, stop, minimum, maximum) < command_size) {
        if (sync_svga_ii() < 0) {
            spin_unlock_irqrestore(&svga_ii_fifo_lock, rflags);
            return -EIO;
        }
        stop = svga_ii_fifo[SVGA_II_FIFO_STOP];
        if (stop < minimum || stop >= maximum || get_svga_ii_fifo_free(next, stop, minimum, maximum) < command_size) {
            spin_unlock_irqrestore(&svga_ii_fifo_lock, rflags);
            return -EIO;
        }
    }

    write_svga_ii_fifo(&next, SVGA_II_CMD_UPDATE);
    write_svga_ii_fifo(&next, (uint32_t)x);
    write_svga_ii_fifo(&next, (uint32_t)y);
    write_svga_ii_fifo(&next, (uint32_t)width);
    write_svga_ii_fifo(&next, (uint32_t)height);
    __asm__ volatile ("" ::: "memory");
    svga_ii_fifo[SVGA_II_FIFO_NEXT_CMD] = next;
    spin_unlock_irqrestore(&svga_ii_fifo_lock, rflags);
    return 0;
}

int set_svga_ii_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp) {
    if (!svga_ii_ready) return -ENODEV;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return -ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    if (xres_virtual != xres || yres_virtual != yres || xoffset || yoffset) return -EOPNOTSUPP;
    if (!xres || !yres || xres > read_svga_ii_register(SVGA_II_REG_MAX_WIDTH) || yres > read_svga_ii_register(SVGA_II_REG_MAX_HEIGHT)) return -EINVAL;
    if (bpp != 32) return -EINVAL;

    uint32_t old_xres = read_svga_ii_register(SVGA_II_REG_WIDTH);
    uint32_t old_yres = read_svga_ii_register(SVGA_II_REG_HEIGHT);
    uint32_t old_bpp = read_svga_ii_register(SVGA_II_REG_BITS_PER_PIXEL);
    uint32_t old_enable = read_svga_ii_register(SVGA_II_REG_ENABLE);

    write_svga_ii_register(SVGA_II_REG_ENABLE, 0);
    write_svga_ii_register(SVGA_II_REG_WIDTH, (uint32_t)xres);
    write_svga_ii_register(SVGA_II_REG_HEIGHT, (uint32_t)yres);
    write_svga_ii_register(SVGA_II_REG_BITS_PER_PIXEL, bpp);
    write_svga_ii_register(SVGA_II_REG_ENABLE, 1);

    uint32_t actual_xres = read_svga_ii_register(SVGA_II_REG_WIDTH);
    uint32_t actual_yres = read_svga_ii_register(SVGA_II_REG_HEIGHT);
    uint32_t actual_bpp = read_svga_ii_register(SVGA_II_REG_BITS_PER_PIXEL);
    uint32_t pitch = read_svga_ii_register(SVGA_II_REG_BYTES_PER_LINE);
    uint32_t offset = read_svga_ii_register(SVGA_II_REG_FB_OFFSET);
    uint32_t framebuffer_size = read_svga_ii_register(SVGA_II_REG_FB_SIZE);

    uint64_t required_size = (uint64_t)pitch * actual_yres;
    if (actual_xres != xres || actual_yres != yres || actual_bpp != bpp || !pitch || offset > svga_ii_vram_size || required_size > svga_ii_vram_size - offset || required_size > framebuffer_size) {
        write_svga_ii_register(SVGA_II_REG_ENABLE, 0);
        write_svga_ii_register(SVGA_II_REG_WIDTH, old_xres);
        write_svga_ii_register(SVGA_II_REG_HEIGHT, old_yres);
        write_svga_ii_register(SVGA_II_REG_BITS_PER_PIXEL, old_bpp);
        write_svga_ii_register(SVGA_II_REG_ENABLE, old_enable);
        log("svga ii: adapter rejected framebuffer mode\n");
        return -EINVAL;
    }

    fb->address = svga_ii_vram + offset;
    fb->width = actual_xres;
    fb->height = actual_yres;
    fb->pitch = pitch;
    fb->bpp = actual_bpp;
    set_svga_ii_mask(read_svga_ii_register(SVGA_II_REG_RED_MASK), &fb->red_mask_size, &fb->red_mask_shift);
    set_svga_ii_mask(read_svga_ii_register(SVGA_II_REG_GREEN_MASK), &fb->green_mask_size, &fb->green_mask_shift);
    set_svga_ii_mask(read_svga_ii_register(SVGA_II_REG_BLUE_MASK), &fb->blue_mask_size, &fb->blue_mask_shift);
    fb_xres_virtual = actual_xres;
    fb_yres_virtual = actual_yres;
    fb_xoffset = 0;
    fb_yoffset = 0;

    sync_terminal();
    if (update_svga_ii(0, 0, actual_xres, actual_yres) == 0) (void)sync_svga_ii();
    return 0;
}

void init_svga_ii(pci_device_t *dev) {
    if (!dev) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    set_pci_d0(dev);
    uint32_t io_bar = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    if (!(io_bar & 0x1) || (io_bar & 0xFFFFFFFCu) > UINT16_MAX - SVGA_II_VALUE_PORT) {
        log("svga ii: invalid register bar\n");
        return;
    }

    uint32_t phys_vram;
    uint32_t phys_fifo;
    if (!get_svga_ii_memory_bar(dev, 0x14, &phys_vram) || !get_svga_ii_memory_bar(dev, 0x18, &phys_fifo)) {
        log("svga ii: invalid memory bars\n");
        return;
    }

    uint32_t command = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, command | 0x7);
    svga_ii_io_base = io_bar & 0xFFFCu;

    write_svga_ii_register(SVGA_II_REG_ID, SVGA_II_ID_2);
    if (read_svga_ii_register(SVGA_II_REG_ID) != SVGA_II_ID_2) {
        svga_ii_io_base = 0;
        log("svga ii: version 2 is unsupported\n");
        return;
    }

    svga_ii_vram_size = read_svga_ii_register(SVGA_II_REG_VRAM_SIZE);
    svga_ii_fifo_size = read_svga_ii_register(SVGA_II_REG_MEM_SIZE);
    if (svga_ii_vram_size < PAGE_SIZE || svga_ii_fifo_size < PAGE_SIZE) {
        log("svga ii: invalid memory size\n");
        return;
    }

    svga_ii_vram = vmap_mmio(phys_vram, (svga_ii_vram_size + PAGE_SIZE - 1) / PAGE_SIZE);
    svga_ii_fifo = vmap_mmio(phys_fifo, (svga_ii_fifo_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!svga_ii_vram || !svga_ii_fifo) {
        svga_ii_vram = NULL;
        svga_ii_fifo = NULL;
        log("svga ii: unable to map device memory\n");
        return;
    }

    if (!initialize_svga_ii_fifo()) {
        log("svga ii: unable to initialize fifo\n");
        return;
    }

    uint64_t xres = fb->width;
    uint64_t yres = fb->height;
    svga_ii_ready = true;
    if (set_svga_ii_resolution(xres, yres, xres, yres, 0, 0, 32) < 0) {
        svga_ii_ready = false;
        return;
    }

    current_fb_driver = FB_SVGA_II;
    log("svga ii: initialized svga ii\n");
}
