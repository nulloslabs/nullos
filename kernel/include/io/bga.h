#pragma once

#include <stdint.h>
#include <io/pci.h>

#define BGA_VENDOR 0x1234
#define BGA_DEVICE 0x1111

#define BGA_IOPORT_INDEX 0x01CE
#define BGA_IOPORT_DATA  0x01CF

#define BGA_DAC_WRITE_INDEX 0x03C8
#define BGA_DAC_DATA        0x03C9

#define BGA_INDEX_ID          0x0
#define BGA_INDEX_XRES        0x1
#define BGA_INDEX_YRES        0x2
#define BGA_INDEX_BPP         0x3
#define BGA_INDEX_ENABLE      0x4
#define BGA_INDEX_BANK        0x5
#define BGA_INDEX_VIRT_WIDTH  0x6
#define BGA_INDEX_VIRT_HEIGHT 0x7
#define BGA_INDEX_X_OFFSET    0x8
#define BGA_INDEX_Y_OFFSET    0x9

#define BGA_DISPI_ID0 0xB0C0
#define BGA_DISPI_ID1 0xB0C1
#define BGA_DISPI_ID2 0xB0C2
#define BGA_DISPI_ID3 0xB0C3
#define BGA_DISPI_ID4 0xB0C4
#define BGA_DISPI_ID5 0xB0C5

#define BGA_DISABLED    0x00
#define BGA_ENABLED     0x01
#define BGA_GETCAPS     0x02
#define BGA_8BIT_DAC    0x20
#define BGA_LFB_ENABLED 0x40
#define BGA_NOCLEARMEM  0x80

uint8_t bga_palette_index(uint32_t color);
uint32_t bga_palette_color(uint8_t index);
void set_bga_fb(uint64_t width, uint64_t height, uint16_t bpp);
void init_bga(pci_device_t *dev);
