#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>
#include <io/ac97.h>
#include <io/io.h>
#include <io/hpet.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <main/spinlock.h>

uint16_t nam_base = 0;
uint16_t nabm_base = 0;
bool ac97_ready = false;
static spinlock_t audio_lock = SPINLOCK_INIT;

static ac97_bd_t bdl[AC97_BDL_SIZE] __attribute__((aligned(8)));
static uint8_t audio_buf[AC97_BDL_SIZE][AC97_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t *audio_src = NULL;
static size_t audio_size = 0;
static size_t audio_offset = 0;

// --- IO Helpers ---
static uint16_t nam_read16(uint8_t reg) { return inw(nam_base + reg); }
static void nam_write16(uint8_t reg, uint16_t v) { outw(nam_base + reg, v); }

static uint8_t nabm_read8(uint8_t reg) { return inb(nabm_base + reg); }
static uint16_t nabm_read16(uint8_t reg) { return inw(nabm_base + reg); }
static uint32_t nabm_read32(uint8_t reg) { return inl(nabm_base + reg); }
static void nabm_write8(uint8_t reg, uint8_t v) { outb(nabm_base + reg, v); }
static void nabm_write16(uint8_t reg, uint16_t v) { outw(nabm_base + reg, v); }
static void nabm_write32(uint8_t reg, uint32_t v) { outl(nabm_base + reg, v); }

// --- Driver Logic ---

// Not meant to be used by kernel, only by interrupts!
void ac97_poll(void) {
    if (!ac97_ready) return;

    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);

    uint16_t sr = nabm_read16(NABM_PCM_OUT_SR);

    // We check for BCIS (Buffer Completion)
    if (sr & 0x1C) { 
        uint8_t civ = nabm_read8(NABM_PCM_OUT_CIV);
        
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
            nabm_write8(NABM_PCM_OUT_CR, 0);
            audio_src = NULL;
            audio_size = 0;
            audio_offset = 0;
        }

        // --- THE CRITICAL FIX ---
        // 1. Tell the compiler/CPU to flush memory to RAM
        asm volatile("mfence" ::: "memory");

        // 2. Update LVI to be ahead of where we are, but not "behind" us.
        // Set LVI to the index we just refilled.
        nabm_write8(NABM_PCM_OUT_LVI, last_index);
        
        // 3. Clear status
        nabm_write16(NABM_PCM_OUT_SR, 0x1C);
    }
    
    spin_unlock_irqrestore(&audio_lock, irq);
}

void init_ac97(pci_device_t *dev) {
    if (!dev) return;

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);

    nam_base  = (uint16_t)(bar0 & 0xFFFC);
    nabm_base = (uint16_t)(bar1 & 0xFFFC);

    // 1. Enable PCI Bus Mastering and IO Space
    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x07);

    // 2. Cold Reset via Global Control
    nabm_write32(NABM_GLOB_CNT, 0x00000002);
    sleep(10);
    nabm_write32(NABM_GLOB_CNT, 0x00000000); 

    // 3. Wait for Codec Ready (Bit 8 of Global Status)
    int timeout = 1000;
    while (!(nabm_read32(NABM_GLOB_STA) & (1 << 8)) && timeout--) {
        sleep(1);
    }

    // 4. Mixer Setup (NAM)
    nam_write16(NAM_RESET, 0xFFFF); // Software Reset
    sleep(10);

    // Power up and wait for the "Analog Ready" bits (0xF)
    nam_write16(NAM_POWERDOWN, 0x0000); 
    timeout = 1000;
    while ((nam_read16(NAM_POWERDOWN) & 0xF) != 0xF && timeout--) {
        sleep(1);
    }

    // Enable External Amplifier (EAPD) - Critical for sound
    nam_write16(NAM_POWERDOWN, nam_read16(NAM_POWERDOWN) | 0x8000);

    // Force Unmute and Max Volume (0x0000 is Max, 0x8000 is Mute)
    nam_write16(NAM_MASTER_VOL, 0x0000);
    nam_write16(NAM_PCM_VOL,    0x0000);

    // 5. Global Control Refinement
    // Enable interrupts (Bit 0) and ensure AC-Link is active
    nabm_write32(NABM_GLOB_CNT, nabm_read32(NABM_GLOB_CNT) | 0x01);

    // 6. Pre-initialize the BDL structure
    for (int i = 0; i < AC97_BDL_SIZE; i++) {
        bdl[i].addr = (uint32_t)virt_to_phys(audio_buf[i]);
        bdl[i].samples = AC97_BUF_SIZE / 2;
        bdl[i].flags = 0x8000; // IOC (Interrupt on Completion)
    }

    register_pci_interrupt_handler(ac97_poll);

    ac97_ready = true;
}

void ac97_play(void *buf, size_t size) {
    if (!ac97_ready || !buf || !size) return;

    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);

    audio_src = (uint8_t *)buf;
    audio_size = size;
    audio_offset = 0;

    nabm_write8(NABM_PCM_OUT_CR, 0x00);
    nabm_write8(NABM_PCM_OUT_CR, 0x02);
    while (nabm_read8(NABM_PCM_OUT_CR) & 0x02);

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

    nabm_write32(NABM_PCM_OUT_BDBAR, (uint32_t)virt_to_phys(bdl));
    nabm_write8(NABM_PCM_OUT_LVI, 31);
    nabm_write16(NABM_PCM_OUT_SR, 0x1C);
    nabm_write8(NABM_PCM_OUT_CR, 0x01 | 0x10);

    spin_unlock_irqrestore(&audio_lock, irq);
}

void ac97_set_volume(uint8_t left, uint8_t right) {
    if (!ac97_ready) return;
    // 0 = Max, 0x1F = Min (for 5-bit) or 0x3F (for 6-bit). 
    // We mask to ensure bit 15 (Mute) stays 0.
    uint16_t vol = ((uint16_t)(63 - (left & 0x3F)) << 8) | (63 - (right & 0x3F));
    nam_write16(NAM_MASTER_VOL, vol);
}

bool ac97_is_playing(void) {
    // Check if the DMA Run bit is still set or if it's halted
    uint8_t sr = nabm_read8(NABM_PCM_OUT_SR);
    return !(sr & (1 << 0)); // DCH (DMA Controller Halted) bit
}