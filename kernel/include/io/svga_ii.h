#pragma once

#include <stdint.h>
#include <io/pci.h>

#define SVGA_II_VENDOR 0x15AD
#define SVGA_II_DEVICE 0x0405

#define SVGA_II_INDEX_PORT 0
#define SVGA_II_VALUE_PORT 1

#define SVGA_II_REG_ID             0
#define SVGA_II_REG_ENABLE         1
#define SVGA_II_REG_WIDTH          2
#define SVGA_II_REG_HEIGHT         3
#define SVGA_II_REG_MAX_WIDTH      4
#define SVGA_II_REG_MAX_HEIGHT     5
#define SVGA_II_REG_BITS_PER_PIXEL 7
#define SVGA_II_REG_RED_MASK       9
#define SVGA_II_REG_GREEN_MASK     10
#define SVGA_II_REG_BLUE_MASK      11
#define SVGA_II_REG_BYTES_PER_LINE 12
#define SVGA_II_REG_FB_OFFSET      14
#define SVGA_II_REG_VRAM_SIZE      15
#define SVGA_II_REG_FB_SIZE        16
#define SVGA_II_REG_MEM_SIZE       19
#define SVGA_II_REG_CONFIG_DONE    20
#define SVGA_II_REG_SYNC           21
#define SVGA_II_REG_BUSY           22
#define SVGA_II_REG_MEM_REGS       30

#define SVGA_II_ID_2 0x90000002

#define SVGA_II_FIFO_MIN      0
#define SVGA_II_FIFO_MAX      1
#define SVGA_II_FIFO_NEXT_CMD 2
#define SVGA_II_FIFO_STOP     3
#define SVGA_II_FIFO_REGS     4

#define SVGA_II_CMD_UPDATE 1
#define SVGA_II_SYNC_LIMIT 10000000

int set_svga_ii_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp);
int update_svga_ii(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
void init_svga_ii(pci_device_t *dev);
