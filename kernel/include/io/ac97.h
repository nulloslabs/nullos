#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>
#include <io/pci.h>

#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

#define NAM_RESET 0x00
#define NAM_MASTER_VOL 0x02
#define NAM_PCM_VOL 0x18
#define NAM_MIC_VOL 0x0E
#define NAM_RECORD_SELECT 0x1A
#define NAM_RECORD_GAIN 0x1C
#define NAM_POWERDOWN 0x26
#define NAM_EXT_AUDIO_ID 0x28
#define NAM_EXT_AUDIO_CTRL 0x2A
#define NAM_PCM_FRONT_DACR 0x2C  // sample rate

#define NABM_PCM_OUT_BDBAR  0x10  // buffer descriptor base address
#define NABM_PCM_OUT_CIV 0x14  // current index value
#define NABM_PCM_OUT_LVI 0x15  // last valid index
#define NABM_PCM_OUT_SR  0x16  // status register
#define NABM_PCM_OUT_PICB 0x18  // position in current buffer
#define NABM_PCM_OUT_CR  0x1B  // control register
#define NABM_GLOB_CNT 0x2C  // global control
#define NABM_GLOB_STA 0x30  // global status

#define CR_RPBM (1 << 0)  // run/pause bus master
#define CR_RR (1 << 1)  // reset registers
#define CR_LVBIE (1 << 2)  // last valid buffer interrupt enable
#define CR_FEIE (1 << 3)  // FIFO error interrupt enable
#define CR_IOCE (1 << 4)  // interrupt on completion enable

#define SR_DCH (1 << 0)  // DMA controller halted
#define SR_CELV (1 << 1)  // current equals last valid
#define SR_LVBCI  (1 << 2)  // last valid buffer completion interrupt
#define SR_BCIS (1 << 3)  // buffer completion interrupt status
#define SR_FIFOE (1 << 4)  // FIFO error

typedef struct {
    uint32_t addr;    // physical address of buffer
    uint16_t samples; // number of samples
    uint16_t flags;   // bit 15 = IOC, bit 14 = BUP
} __attribute__((packed)) ac97_bd_t;

#define BD_IOC (1 << 15)  // interrupt on completion
#define BD_BUP (1 << 14)  // buffer underrun policy (play silence)

#define AC97_BUF_SIZE 8192  // bytes per buffer descriptor entry
#define AC97_BDL_SIZE 32

extern uint16_t nam_base;
extern uint16_t nabm_base;
extern bool ac97_ready;

void init_ac97(pci_device_t *dev);
bool ac97_is_ready(void);
void ac97_set_volume(uint8_t left, uint8_t right); // 0 = mute, 63 = max
void ac97_play(void *buf, size_t size);
void ac97_stop(void);
bool ac97_is_playing(void);
uint16_t ac97_get_sample_rate(void);