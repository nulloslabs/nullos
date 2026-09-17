#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <io/audio.h>
#include <io/pci.h>

#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

#define AC97_BUF_SIZE    8192
#define AC97_BDL_SIZE    32
#define AC97_WIRE_FRAMES 2048

#define AC97_BYTE_RATE     192000ULL
#define AC97_PACE_AHEAD_US 85000ULL

#define AC97_STALL_TIMEOUT_US 5000000ULL

#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER_VOL     0x02
#define AC97_NAM_PCM_VOL        0x18
#define AC97_NAM_MIC_VOL        0x0E
#define AC97_NAM_RECORD_SELECT  0x1A
#define AC97_NAM_RECORD_GAIN    0x1C
#define AC97_NAM_POWERDOWN      0x26
#define AC97_NAM_EXT_AUDIO_ID   0x28
#define AC97_NAM_EXT_AUDIO_CTRL 0x2A
#define AC97_NAM_PCM_FRONT_DACR 0x2C

#define AC97_NABM_PCM_OUT_BDBAR 0x10
#define AC97_NABM_PCM_OUT_CIV   0x14
#define AC97_NABM_PCM_OUT_LVI   0x15
#define AC97_NABM_PCM_OUT_SR    0x16
#define AC97_NABM_PCM_OUT_PICB  0x18
#define AC97_NABM_PCM_OUT_CR    0x1B
#define AC97_NABM_GLOB_CNT      0x2C
#define AC97_NABM_GLOB_STA      0x30

typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bd_t;

extern uint16_t nam_base;
extern uint16_t nabm_base;

bool is_ac97_playing(void);
bool is_ac97_started(void);
bool is_ac97_muted(void);
int set_ac97_volume(uint8_t left, uint8_t right);
void get_ac97_volume(uint8_t *left, uint8_t *right);
void get_ac97_driver_info(audio_driver_info_t *info);
int mute_ac97(bool muted);
int set_ac97_params(const audio_params_t *params);
uint32_t get_ac97_rate(void);
uint64_t get_ac97_staged_frames(void);
uint64_t get_ac97_pending_frames(void);
uint64_t get_ac97_ring_bytes(void);
uint64_t get_ac97_ring_phys(void);
uint64_t get_ac97_buffer_frames(void);
uint64_t get_ac97_period_frames(void);
uint32_t get_ac97_max_period_frames(void);
int set_ac97_start_threshold(uint64_t frames);
int commit_ac97_frames(uint64_t appl_frames);
int kick_ac97(void);
void reset_ac97_mmap(void);
int flush_ac97(uint64_t appl_frames);
void drop_ac97(void);
int play_ac97(void *buf, size_t size);
void init_ac97(pci_device_t *dev);
