#pragma once

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void mask_pic_irq(uint8_t irq);
void unmask_pic_irq(uint8_t irq);
void eoi_pic(void);
void disable_pic(void);
void remap_pic(void);
