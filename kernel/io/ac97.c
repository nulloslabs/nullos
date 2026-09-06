#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/ac97.h>
#include <io/io.h>
#include <io/time.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

uint16_t nam_base = 0;
uint16_t nabm_base = 0;
static bool ac97_ready = false;
static spinlock_t audio_lock = SPINLOCK_INIT;

static ac97_bd_t *bdl;
static uint8_t (*audio_buf)[AC97_BUF_SIZE];
static uint8_t *audio_src = NULL;
static size_t audio_size = 0;
static size_t audio_offset = 0;

// --- IO Helpers ---
static uint16_t read_nam16(uint8_t reg) { return inw(nam_base + reg); }
static void write_nam16(uint8_t reg, uint16_t v) { outw(nam_base + reg, v); }
static uint8_t read_nabm8(uint8_t reg) { return inb(nabm_base + reg); }
static uint16_t read_nabm16(uint8_t reg) { return inw(nabm_base + reg); }
static uint32_t read_nabm32(uint8_t reg) { return inl(nabm_base + reg); }
static void write_nabm8(uint8_t reg, uint8_t v) { outb(nabm_base + reg, v); }
static void write_nabm16(uint8_t reg, uint16_t v) { outw(nabm_base + reg, v); }
static void write_nabm32(uint8_t reg, uint32_t v) { outl(nabm_base + reg, v); }

// Not meant to be used by kernel, only by interrupts!
static void poll_ac97(void) {
    if (!ac97_ready) return;

    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);

    uint16_t sr = read_nabm16(NABM_PCM_OUT_SR);

    // We check for BCIS (Buffer Completion)
    if (sr & 0x1C) { 
        uint8_t civ = read_nabm8(NABM_PCM_OUT_CIV);
        
        // Refill the slot that was just vacated
        int last_index = (civ + 31) % AC97_BDL_SIZE; 

        if (audio_offset < audio_size) {
            size_t remaining = audio_size - audio_offset;
            size_t chunk = remaining < AC97_BUF_SIZE ? remaining : AC97_BUF_SIZE;
            memcpy(audio_buf[last_index], audio_src + audio_offset, chunk);
            if (chunk < AC97_BUF_SIZE)
                memset(audio_buf[last_index] + chunk, 0, AC97_BUF_SIZE - chunk);
            bdl[last_index].addr    = (uint32_t)virt_to_phys(audio_buf[last_index]);
            bdl[last_index].samples = AC97_BUF_SIZE / 2;
            audio_offset += chunk;
        } else {
            write_nabm8(NABM_PCM_OUT_CR, 0);
            audio_src = NULL;
            audio_size = 0;
            audio_offset = 0;
        }
        __asm__ volatile ("mfence" ::: "memory");
        write_nabm8(NABM_PCM_OUT_LVI, last_index);
        write_nabm16(NABM_PCM_OUT_SR, 0x1C);
    }
    
    spin_unlock_irqrestore(&audio_lock, irq);
}

bool is_ac97_playing(void) {
    // Check if the DMA Run bit is still set or if it's halted
    uint8_t sr = read_nabm8(NABM_PCM_OUT_SR);
    return !(sr & (1 << 0)); // DCH (DMA Controller Halted) bit
}

void play_ac97(void *buf, size_t size) {
    if (!ac97_ready || !buf || !size) return;

    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);

    audio_src = (uint8_t *)buf;
    audio_size = size;
    audio_offset = 0;

    write_nabm8(NABM_PCM_OUT_CR, 0x00);
    write_nabm8(NABM_PCM_OUT_CR, 0x02);
    while (read_nabm8(NABM_PCM_OUT_CR) & 0x02);

    for (int i = 0; i < AC97_BDL_SIZE; i++) {
        size_t remaining = audio_size - audio_offset;
        size_t chunk = remaining < AC97_BUF_SIZE ? remaining : AC97_BUF_SIZE;
        memcpy(audio_buf[i], audio_src + audio_offset, chunk);
        if (chunk < AC97_BUF_SIZE)
            memset(audio_buf[i] + chunk, 0, AC97_BUF_SIZE - chunk);
        bdl[i].addr    = (uint32_t)virt_to_phys(audio_buf[i]);
        bdl[i].samples = AC97_BUF_SIZE / 2;
        bdl[i].flags   = 0x8000;
        audio_offset += chunk;
        // DON'T wrap here — let IRQ handle continuation
    }

    write_nabm32(NABM_PCM_OUT_BDBAR, (uint32_t)virt_to_phys(bdl));
    write_nabm8(NABM_PCM_OUT_LVI, 31);
    write_nabm16(NABM_PCM_OUT_SR, 0x1C);
    write_nabm8(NABM_PCM_OUT_CR, 0x01 | 0x10);

    spin_unlock_irqrestore(&audio_lock, irq);
}

void set_ac97_volume(uint8_t left, uint8_t right) {
    if (!ac97_ready) return;
    // 0 = Max, 0x1F = Min (for 5-bit) or 0x3F (for 6-bit). 
    // We mask to ensure bit 15 (Mute) stays 0.
    uint16_t vol = ((uint16_t)(63 - (left & 0x3F)) << 8) | (63 - (right & 0x3F));
    write_nam16(NAM_MASTER_VOL, vol);
}

void init_ac97(pci_device_t *dev) {
    if (!dev) return;

    uint64_t dma_pages = 1 + ((uint64_t)AC97_BDL_SIZE * AC97_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    void *dma_phys = prealloc_dma32(dma_pages);
    if (!dma_phys) { log("ac97: unable to allocate dma memory\n"); return; }
    uint8_t *dma_virt = phys_to_virt((uint64_t)dma_phys);
    bdl = (ac97_bd_t *)dma_virt;
    audio_buf = (uint8_t (*)[AC97_BUF_SIZE])(dma_virt + PAGE_SIZE);

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);

    nam_base  = (uint16_t)(bar0 & 0xFFFC);
    nabm_base = (uint16_t)(bar1 & 0xFFFC);

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x07);

    write_nabm32(NABM_GLOB_CNT, 0x00000002);
    sleep(10);
    write_nabm32(NABM_GLOB_CNT, 0x00000000);

    int timeout = 1000;
    while (!(read_nabm32(NABM_GLOB_STA) & (1 << 8)) && timeout--) {
        sleep(1);
    }

    write_nam16(NAM_RESET, 0xFFFF);
    sleep(10);

    write_nam16(NAM_POWERDOWN, 0x0000);
    timeout = 1000;
    while ((read_nam16(NAM_POWERDOWN) & 0xF) != 0xF && timeout--) {
        sleep(1);
    }

    // EAPD, bit 15, powers the external speaker amp
    write_nam16(NAM_POWERDOWN, read_nam16(NAM_POWERDOWN) | 0x8000);
    write_nam16(NAM_MASTER_VOL, 0x0000);
    write_nam16(NAM_PCM_VOL, 0x0000);

    // GPIE, bit 0, keeps the ac link active
    write_nabm32(NABM_GLOB_CNT, read_nabm32(NABM_GLOB_CNT) | 0x01);

    for (int i = 0; i < AC97_BDL_SIZE; i++) {
        bdl[i].addr = (uint32_t)virt_to_phys(audio_buf[i]);
        bdl[i].samples = AC97_BUF_SIZE / 2;
        bdl[i].flags = 0x8000; // IOC
    }

    pci_request_irq(dev, poll_ac97);

    log("ac97: initialized ac97\n");

    ac97_ready = true;
}
