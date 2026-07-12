#pragma once

void eoi_pic(void);
void disable_pic(void);
void mask_pic_irq(uint8_t irq);
void unmask_pic_irq(uint8_t irq);
void remap_pic(void);
