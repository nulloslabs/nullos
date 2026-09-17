#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <io/ac97.h>
#include <io/audio.h>
#include <io/time.h>
#include <io/hpet.h>
#include <io/io.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

uint16_t nam_base = 0;
uint16_t nabm_base = 0;
static bool ac97_ready = false;
static spinlock_t audio_lock = SPINLOCK_INIT;

static ac97_bd_t *bdl;
static uint8_t (*audio_buf)[AC97_BUF_SIZE];
static uint64_t ring_phys;

static int pos = 0;          // next BDL slot to fill (== (LVI+1)%32)
static bool running = false; // engine kicked; RUN stays set, halts resume via LVI

// Wire is always stereo S16LE; anything else is converted into the ring.
// BDL entries are re-pointed per staged slot (see aim_bdl_locked).
static audio_params_t active_params = { 48000, 2, SNDRV_PCM_FORMAT_S16_LE, AC97_WIRE_FRAMES, AC97_BDL_SIZE - 1 };
static uint32_t period_frames = AC97_WIRE_FRAMES;
static uint32_t buffer_periods = AC97_BDL_SIZE - 1;
static size_t frame_bytes = 4;
static uint32_t wire_rate = AC97_BYTE_RATE;
static uint8_t frame_carry[4];
static size_t carry_len = 0;
static uint8_t slot_in[AC97_BUF_SIZE];
static int16_t asm_slot[AC97_WIRE_FRAMES][2];
static size_t asm_frames = 0;
static uint64_t staged_total = 0;
static uint64_t start_threshold_frames = 1; // auto-start once staged past this

static uint64_t pace_anchor_us = 0; // HPET time of the kick
static uint64_t pace_bytes = 0;     // payload bytes staged since the kick

static void pace_producer(size_t staged) {
    (void)staged;
    (void)pace_anchor_us;
    (void)pace_bytes;
    (void)wire_rate;
    // Disabled: snd_pcm_write blocking already throttles to realtime;
    // spinning on MMIO here is a VM-exit storm under KVM.
    return;
}

static void convert_frames(const uint8_t *in, int16_t (*out)[2], size_t frames) {
    if (active_params.channels == 2 && active_params.format == SNDRV_PCM_FORMAT_S16_LE) { memcpy(out, in, frames * 4); return; }
    for (size_t f = 0; f < frames; f++) {
        int16_t left;
        int16_t right;
        if (active_params.format == SNDRV_PCM_FORMAT_S16_LE) {
            int16_t s0;
            memcpy(&s0, in + f * active_params.channels * 2, 2);
            left = s0;
            right = s0;
            if (active_params.channels == 2) memcpy(&right, in + (f * 2 + 1) * 2, 2);
        } else {
            const uint8_t *s = in + f * active_params.channels;
            left = (int16_t)(((int)s[0] - 128) << 8);
            if (active_params.channels == 2) right = (int16_t)(((int)s[1] - 128) << 8); else right = left;
        }
        out[f][0] = left;
        out[f][1] = right;
    }
}

static uint16_t ac97_vol = 0x0000; // staged attenuation (bit 15 always 0 here)
static bool ac97_muted = false;

// --- IO Helpers ---
static uint16_t read_nam16(uint8_t reg) { return inw(nam_base + reg); }
static void write_nam16(uint8_t reg, uint16_t v) { outw(nam_base + reg, v); }
static uint8_t read_nabm8(uint8_t reg) { return inb(nabm_base + reg); }
static uint16_t read_nabm16(uint8_t reg) { return inw(nabm_base + reg); }
static uint32_t read_nabm32(uint8_t reg) { return inl(nabm_base + reg); }
static void write_nabm8(uint8_t reg, uint8_t v) { outb(nabm_base + reg, v); }
static void write_nabm16(uint8_t reg, uint16_t v) { outw(nabm_base + reg, v); }
static void write_nabm32(uint8_t reg, uint32_t v) { outl(nabm_base + reg, v); }

static int outstanding(void) {
    uint8_t civ = read_nabm8(AC97_NABM_PCM_OUT_CIV);
    return (pos - civ + AC97_BDL_SIZE) % AC97_BDL_SIZE;
}

// The DMA ring as flat bytes: buffer_periods * period_frames * 4 at the
// front of the audio_buf allocation.
static uint8_t *ring_base(void) {
    return (uint8_t *)audio_buf;
}

static uint64_t buffer_frames_total(void) {
    return (uint64_t)buffer_periods * period_frames;
}

// The BDL walk wraps at 32 slots but the ring wraps at buffer_periods, so
// each staged slot must re-point its BDL entry (plain aliasing only works
// when buffer_periods divides 32).
static void aim_bdl_locked(int slot) {
    bdl[slot].addr = (uint32_t)(ring_phys + PAGE_SIZE + (uint64_t)(staged_total % buffer_frames_total()) * 4);
}

// Debug: rate-limited log when the engine underran before we exposed a period.
// Called under audio_lock.
static uint64_t last_halt_log_us = 0;
static void log_engine_halt_locked(const char *via) {
    if (!(read_nabm8(AC97_NABM_PCM_OUT_SR) & 0x01)) return; // DCH
    uint64_t now = get_elapsed_hpet_us();
    if (last_halt_log_us && now - last_halt_log_us < 100000) return;
    last_halt_log_us = now;
    log("ac97: engine had halted (underrun) at %s: staged=%lu out=%d\n", via, staged_total, outstanding());
}

// Bounded wait for the RR bit to clear (no HPET -> never times out).
static bool wait_rr_clear(void) {
    uint64_t start = get_elapsed_hpet_us();
    while (read_nabm8(AC97_NABM_PCM_OUT_CR) & 0x02) {
        if (get_elapsed_hpet_us() - start > AC97_STALL_TIMEOUT_US) {
            log("ac97: rr reset stuck\n");
            return false;
        }
        __asm__ volatile ("pause");
    }
    return true;
}

static void start_dma_locked(void) {
    write_nabm8(AC97_NABM_PCM_OUT_CR, 0x00);
    write_nabm8(AC97_NABM_PCM_OUT_CR, 0x02);
    wait_rr_clear();

    write_nabm32(AC97_NABM_PCM_OUT_BDBAR, (uint32_t)virt_to_phys(bdl));
    write_nabm16(AC97_NABM_PCM_OUT_SR, 0x1C);
    // I/O writes are ordered after prior stores; no fence needed.
    write_nabm8(AC97_NABM_PCM_OUT_LVI, (pos + AC97_BDL_SIZE - 1) % AC97_BDL_SIZE);
    // Run + LVBIE + IOCE
    write_nabm8(AC97_NABM_PCM_OUT_CR, 0x01 | 0x04 | 0x10);

    // Anchor realtime pacing here: the already-staged slots count from now.
    pace_anchor_us = get_elapsed_hpet_us();
    pace_bytes = 0;
    running = true;
}

static void poll_ac97(void) {
    if (!ac97_ready) return;

    uint16_t sr = read_nabm16(AC97_NABM_PCM_OUT_SR);
    if (sr & 0x1C) write_nabm16(AC97_NABM_PCM_OUT_SR, 0x1C);
}

bool is_ac97_playing(void) {
    int out = outstanding();
    if (out > 1) return true;
    uint8_t sr = read_nabm8(AC97_NABM_PCM_OUT_SR);
    return !(sr & (1 << 0)); // DCH (DMA Controller Halted) bit
}

// True once kicked, even while the engine sits halted with RUN still set.
bool is_ac97_started(void) {
    return running;
}

bool is_ac97_muted(void) {
    return ac97_muted;
}

uint32_t get_ac97_rate(void) {
    return active_params.rate;
}

int set_ac97_params(const audio_params_t *params) {
    if (!ac97_ready) return -ENODEV;
    if (!params) return -EINVAL;
    if (params->channels < 1 || params->channels > 2) return -EINVAL;
    if (params->format != SNDRV_PCM_FORMAT_U8 && params->format != SNDRV_PCM_FORMAT_S16_LE) return -EINVAL;
    if (params->rate < 8000 || params->rate > 48000) return -EINVAL;
    uint32_t pf = params->period_frames ? params->period_frames : AC97_WIRE_FRAMES;
    uint32_t bp = params->buffer_periods ? params->buffer_periods : AC97_BDL_SIZE - 1;
    if (pf == 0 || pf > AC97_WIRE_FRAMES) return -EINVAL;
    if (bp == 0 || bp > AC97_BDL_SIZE - 1) return -EINVAL;
    if ((uint64_t)pf * bp > AC97_BDL_SIZE * AC97_BUF_SIZE / 4) return -EINVAL;
    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);
    if (is_ac97_playing()) { spin_unlock_irqrestore(&audio_lock, irq); return -EBUSY; }
    if (params->rate != 48000) {
        if (!(read_nam16(AC97_NAM_EXT_AUDIO_ID) & 0x01)) { spin_unlock_irqrestore(&audio_lock, irq); return -EOPNOTSUPP; }
        // VRA: variable-rate audio (QEMU's STAC9700 has it).
        write_nam16(AC97_NAM_EXT_AUDIO_CTRL, read_nam16(AC97_NAM_EXT_AUDIO_CTRL) | 0x01);
    }
    write_nam16(AC97_NAM_PCM_FRONT_DACR, (uint16_t)params->rate);
    active_params = *params;
    active_params.period_frames = pf;
    active_params.buffer_periods = bp;
    period_frames = pf;
    buffer_periods = bp;
    frame_bytes = (size_t)params->channels * (params->format == SNDRV_PCM_FORMAT_U8 ? 1 : 2);
    wire_rate = params->rate * 4;
    uint32_t period_bytes = pf * 4;
    for (int d = 0; d < AC97_BDL_SIZE; d++) {
        bdl[d].addr = (uint32_t)(ring_phys + PAGE_SIZE + (uint64_t)(d % (int)bp) * period_bytes);
        bdl[d].samples = period_bytes / 2;
        bdl[d].flags = 0x8000; // IOC
    }
    pos = 0;
    carry_len = 0;
    asm_frames = 0;
    staged_total = 0;
    start_threshold_frames = 1; // Linux default sw param
    running = false;            // engine is idle here (EBUSY above)
    pace_anchor_us = get_elapsed_hpet_us();
    pace_bytes = 0;
    spin_unlock_irqrestore(&audio_lock, irq);
    return 0;
}

void drop_ac97(void) {
    if (!ac97_ready) return;
    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);
    write_nabm8(AC97_NABM_PCM_OUT_CR, 0x00);
    write_nabm8(AC97_NABM_PCM_OUT_CR, 0x02);
    wait_rr_clear();
    pos = 0;
    carry_len = 0;
    asm_frames = 0;
    staged_total = 0;
    running = false;
    spin_unlock_irqrestore(&audio_lock, irq);
}

uint64_t get_ac97_staged_frames(void) {
    return staged_total;
}

uint64_t get_ac97_ring_bytes(void) {
    return (uint64_t)buffer_periods * period_frames * 4;
}

uint64_t get_ac97_ring_phys(void) {
    if (!ac97_ready) return 0;
    return ring_phys + PAGE_SIZE;
}

// Max buffer frames: 31 BDL slots of the largest period.
uint64_t get_ac97_buffer_frames(void) {
    return (uint64_t)(AC97_BDL_SIZE - 1) * AC97_WIRE_FRAMES;
}

uint64_t get_ac97_period_frames(void) {
    return period_frames;
}

uint32_t get_ac97_max_period_frames(void) {
    return AC97_WIRE_FRAMES;
}

int set_ac97_start_threshold(uint64_t frames) {
    start_threshold_frames = frames;
    return 0;
}

static void stage_slot_locked(void);

// Silence the whole period at the producer position (appl == staged here).
static void pad_period_locked(void) {    uint64_t buffer_frames = (uint64_t)buffer_periods * period_frames;
    memset(ring_base() + (staged_total % buffer_frames) * 4, 0, period_frames * 4);
    aim_bdl_locked(pos);
    pos = (pos + 1) % AC97_BDL_SIZE;
    staged_total += period_frames;
}

// Expose whole periods up to appl_frames; no copy, just LVI bumps.
int commit_ac97_frames(uint64_t appl_frames) {
    if (!ac97_ready) return -ENODEV;
    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);
    while (appl_frames >= staged_total + period_frames) {
        if (outstanding() >= AC97_BDL_SIZE - 1) break;
        int filled = pos;
        aim_bdl_locked(filled);
        pos = (pos + 1) % AC97_BDL_SIZE;
        staged_total += period_frames;
        if (!running) {
            if (staged_total >= start_threshold_frames) start_dma_locked();
        } else {
            log_engine_halt_locked("commit");
            write_nabm8(AC97_NABM_PCM_OUT_LVI, filled);
        }
    }
    spin_unlock_irqrestore(&audio_lock, irq);
    return 0;
}

// Start now; pad a partial tail period so a tiny write still makes sound.
int kick_ac97(void) {
    if (!ac97_ready) return -ENODEV;
    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);
    if (!running && outstanding() == 0 && asm_frames > 0) {
        memset((uint8_t *)asm_slot + asm_frames * 4, 0, period_frames * 4 - asm_frames * 4);
        stage_slot_locked();
    }
    if (!running && outstanding() > 0) {
        if (outstanding() < 2 && buffer_periods >= 2) pad_period_locked();
        start_dma_locked();
    }
    spin_unlock_irqrestore(&audio_lock, irq);
    return 0;
}

// Drop everything and scrub the ring so never-written slots play silence.
void reset_ac97_mmap(void) {
    if (!ac97_ready) return;
    drop_ac97();
    memset(audio_buf, 0, (uint64_t)AC97_BDL_SIZE * AC97_BUF_SIZE);
}

// PICB is frames remaining in the CIV slot (0 when self-halted), so
// pending = (out-1)*period + PICB is exact in every engine state.
uint64_t get_ac97_pending_frames(void) {
    if (!running) return 0;
    int out = outstanding();
    if (out == 0) return 0;
    uint64_t picb = read_nabm16(AC97_NABM_PCM_OUT_PICB);
    if (picb >= period_frames) picb = period_frames;
    return (uint64_t)(out - 1) * period_frames + picb;
}

// Hands one full assembly period to the engine. Call with room in the BDL.
static void stage_slot_locked(void) {
    int filled = pos;
    memcpy(ring_base() + (staged_total % buffer_frames_total()) * 4, asm_slot, period_frames * 4);
    aim_bdl_locked(filled);
    pos = (pos + 1) % AC97_BDL_SIZE;
    staged_total += period_frames;
    asm_frames = 0;
    if (!running) {
        if (staged_total >= start_threshold_frames) start_dma_locked();
    } else {
        log_engine_halt_locked("stage");
        write_nabm8(AC97_NABM_PCM_OUT_LVI, filled);
    }
}

int play_ac97(void *buf, size_t size) {
    if (!ac97_ready || !buf || !size) return 0;

    const uint8_t *p = buf;
    uint64_t irq;

    while (size > 0) {
        // BDL full: sleep (not spin; VM-exit storm under KVM) until the
        // engine drains a slot. Bounded stall timeout.
        uint64_t stall_start = get_elapsed_hpet_us();
        while (outstanding() >= AC97_BDL_SIZE - 1) {
            if (get_elapsed_hpet_us() - stall_start > AC97_STALL_TIMEOUT_US) {
                log("ac97: engine stalled\n");
                return -ETIMEDOUT;
            }
            let_current_task_sleep(1000);
        }

        size_t staged = 0;
        spin_lock_irqsave(&audio_lock, &irq);
        if (outstanding() < AC97_BDL_SIZE - 1) {
            // Assemble input frames into the pending wire period.
            size_t want = (period_frames - asm_frames) * frame_bytes;
            size_t n = 0;
            if (carry_len > 0) { memcpy(slot_in, frame_carry, carry_len); n = carry_len; carry_len = 0; }
            size_t take = size < want - n ? size : want - n;
            memcpy(slot_in + n, p, take);
            p += take;
            size -= take;
            n += take;
            size_t frames = n / frame_bytes;
            size_t tail = n - frames * frame_bytes;
            if (tail > 0) { memcpy(frame_carry, slot_in + frames * frame_bytes, tail); carry_len = tail; }
            if (frames == 0) { spin_unlock_irqrestore(&audio_lock, irq); continue; }
            convert_frames(slot_in, asm_slot + asm_frames, frames);
            asm_frames += frames;
            if (asm_frames < period_frames) { spin_unlock_irqrestore(&audio_lock, irq); continue; }
            stage_slot_locked();
            staged = period_frames * 4;
        }
        spin_unlock_irqrestore(&audio_lock, irq);

        // Throttle staged-ahead bytes to realtime (outside the lock so no
        // long cli stretch). Pre-kick staging is a few periods: skip it.
        if (running && staged > 0) pace_producer(staged);
    }

    // asm_frames carries over; DRAIN seals the tail (see flush_ac97).
    return 0;
}

// Drain: expose committed periods, seal the sub-period tail with silence
// so partial final periods play out like Linux.
int flush_ac97(uint64_t appl_frames) {
    if (!ac97_ready) return 0;
    commit_ac97_frames(appl_frames); // whole periods; no-op if sealed
    uint64_t irq;
    spin_lock_irqsave(&audio_lock, &irq);
    carry_len = 0;
    if (appl_frames > staged_total) {
        // Partial tail: end silenced. Sealed here, not on commit, since
        // the producer may still be filling this slot.
        uint64_t rem = appl_frames - staged_total;
        if (rem > period_frames) rem = period_frames;
        uint64_t stall_start = get_elapsed_hpet_us();
        while (outstanding() >= AC97_BDL_SIZE - 1) {
            spin_unlock_irqrestore(&audio_lock, irq);
            if (get_elapsed_hpet_us() - stall_start > AC97_STALL_TIMEOUT_US) {
                log("ac97: engine stalled\n");
                return -ETIMEDOUT;
            }
            let_current_task_sleep(1000);
            spin_lock_irqsave(&audio_lock, &irq);
        }
        uint8_t *slot = ring_base() + (staged_total % buffer_frames_total()) * 4;
        if (asm_frames > 0) {
            size_t have = asm_frames;
            if (have > rem) have = (size_t)rem;
            memcpy(slot, asm_slot, have * 4);
            asm_frames = 0;
        }
        memset(slot + rem * 4, 0, (period_frames - (uint32_t)rem) * 4);
        int filled = pos;
        aim_bdl_locked(filled);
        pos = (pos + 1) % AC97_BDL_SIZE;
        staged_total += period_frames;
        if (!running) {
            if (staged_total >= start_threshold_frames) start_dma_locked();
        } else {
            log_engine_halt_locked("flush");
            write_nabm8(AC97_NABM_PCM_OUT_LVI, filled);
        }
    }
    if (!running && outstanding() > 0) {
        if (outstanding() < 2 && buffer_periods >= 2) pad_period_locked();
        start_dma_locked();
    }
    spin_unlock_irqrestore(&audio_lock, irq);
    uint64_t drain_start = get_elapsed_hpet_us();
    while (is_ac97_playing()) {
        if (get_elapsed_hpet_us() - drain_start > AC97_STALL_TIMEOUT_US) {
            log("ac97: engine stalled\n");
            return -ETIMEDOUT;
        }
        let_current_task_sleep(2000);
    }
    return 0;
}

int set_ac97_volume(uint8_t left, uint8_t right) {
    if (!ac97_ready) return -ENODEV;
    // 0 = max, 0x3F = min; keep mute bit 15 clear.
    uint16_t vol = ((uint16_t)(63 - (left & 0x3F)) << 8) | (63 - (right & 0x3F));
    ac97_vol = vol;
    write_nam16(AC97_NAM_MASTER_VOL, vol | (ac97_muted ? 0x8000 : 0));
    return 0;
}

int mute_ac97(bool muted) {
    if (!ac97_ready) return -ENODEV;
    ac97_muted = muted;
    write_nam16(AC97_NAM_MASTER_VOL, ac97_vol | (muted ? 0x8000 : 0));
    return 0;
}

void get_ac97_volume(uint8_t *left, uint8_t *right) {
    if (left) *left = (uint8_t)(63 - ((ac97_vol >> 8) & 0x3F));
    if (right) *right = (uint8_t)(63 - (ac97_vol & 0x3F));
}

void get_ac97_driver_info(audio_driver_info_t *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, "ac97", sizeof(info->name) - 1);
    strncpy(info->longname, "Nullkrnl AC'97 at PCI", sizeof(info->longname) - 1);
    strncpy(info->mixername, "AC'97", sizeof(info->mixername) - 1);
    strncpy(info->components, "AC'97", sizeof(info->components) - 1);
    strncpy(info->pcm_name, "AC'97 Playback", sizeof(info->pcm_name) - 1);
}

void init_ac97(pci_device_t *dev) {
    if (!dev) return;

    uint64_t dma_pages = 1 + ((uint64_t)AC97_BDL_SIZE * AC97_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    void *dma_phys = prealloc_dma32(dma_pages);
    if (!dma_phys) { log("ac97: unable to allocate dma memory\n"); return; }
    ring_phys = (uint64_t)dma_phys;
    uint8_t *dma_virt = phys_to_virt((uint64_t)dma_phys);
    bdl = (ac97_bd_t *)dma_virt;
    audio_buf = (uint8_t (*)[AC97_BUF_SIZE])(dma_virt + PAGE_SIZE);

    uint32_t bar0 = read_pci(dev->bus, dev->dev, dev->func, 0x10);
    uint32_t bar1 = read_pci(dev->bus, dev->dev, dev->func, 0x14);

    nam_base = (uint16_t)(bar0 & 0xFFFC);
    nabm_base = (uint16_t)(bar1 & 0xFFFC);

    uint32_t cmd = read_pci(dev->bus, dev->dev, dev->func, 0x04);
    write_pci(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x07);

    write_nabm32(AC97_NABM_GLOB_CNT, 0x00000002);
    sleep(10);
    write_nabm32(AC97_NABM_GLOB_CNT, 0x00000000);

    int timeout = 1000;
    while (!(read_nabm32(AC97_NABM_GLOB_STA) & (1 << 8)) && timeout--) {
        sleep(1);
    }

    write_nam16(AC97_NAM_RESET, 0xFFFF);
    sleep(10);

    write_nam16(AC97_NAM_POWERDOWN, 0x0000);
    timeout = 1000;
    while ((read_nam16(AC97_NAM_POWERDOWN) & 0xF) != 0xF && timeout--) {
        sleep(1);
    }

    // EAPD, bit 15, powers the external speaker amp
    write_nam16(AC97_NAM_POWERDOWN, read_nam16(AC97_NAM_POWERDOWN) | 0x8000);
    write_nam16(AC97_NAM_MASTER_VOL, 0x0000);
    write_nam16(AC97_NAM_PCM_VOL, 0x0000);

    // GPIE, bit 0, keeps the ac link active
    write_nabm32(AC97_NABM_GLOB_CNT, read_nabm32(AC97_NABM_GLOB_CNT) | 0x01);

    for (int i = 0; i < AC97_BDL_SIZE; i++) {
        bdl[i].addr = (uint32_t)virt_to_phys(audio_buf[i]);
        bdl[i].samples = AC97_BUF_SIZE / 2;
        bdl[i].flags = 0x8000; // IOC
    }

    request_pci_irq(dev, poll_ac97);

    current_audio_driver = AUDIO_AC97;
    ac97_ready = true;

    log("ac97: initialized ac97\n");
}
