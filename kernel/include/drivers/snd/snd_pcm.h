#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sound/asound.h>

#define SND_PCM_DEFAULT_AVAIL_MIN       2048
#define SND_PCM_DEFAULT_START_THRESHOLD 1

#define PCM_PERIOD_MIN_FRAMES 32
#define PCM_PERIOD_MAX_FRAMES 2048
#define PCM_MAX_PERIODS       31
#define PCM_STALL_TIMEOUT_US  5000000ULL

#define SND_PCM_HW_ACCESS_MASK ((1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED) | (1u << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED))
#define SND_PCM_HW_FORMAT_MASK ((1u << SNDRV_PCM_FORMAT_U8) | (1u << SNDRV_PCM_FORMAT_S16_LE))
#define SND_PCM_HW_RATE_MIN    8000
#define SND_PCM_HW_RATE_MAX    48000

typedef struct {
    uint32_t rate;
    uint8_t channels;
    int format;
    bool configured;
    snd_pcm_state_t state;
    uint64_t appl_frames; // free-running producer position
    uint64_t buffer_frames;
    uint64_t boundary;    // power-of-2 ring for hw/appl modulo
} snd_pcm_stream_t;

typedef struct {
    int card;
    bool mmap_mode;
    uint64_t appl_frames;
    uint64_t boundary;
    uint64_t avail_min;
    uint64_t start_threshold;
    uint64_t stop_threshold;
    uint64_t status_phys;
    uint64_t control_phys;
} snd_pcm_handle_t;

int register_snd_pcm(int card, int device);
uint64_t read_snd_pcm(void *buf, uint64_t count, uint64_t offset, int index, void *handle);
uint64_t write_snd_pcm(const void *buf, uint64_t count, uint64_t offset, int index, void *handle);
void *open_snd_pcm(int index);
void release_snd_pcm(void *handle);
bool query_snd_pcm_mmap(const char *rel, void *handle, uint64_t offset, uint64_t *phys_out, uint64_t *pages_out);
int refine_snd_pcm(int card, struct snd_pcm_hw_params *params);
int set_snd_pcm_params(int card, void *handle, struct snd_pcm_hw_params *params);
int set_snd_pcm_sw(void *handle, struct snd_pcm_sw_params *params);
int sync_snd_pcm(void *handle, struct snd_pcm_sync_ptr *sync);
int get_snd_pcm_info(int card, struct snd_pcm_info *info);
int get_snd_pcm_status(int card, void *handle, struct snd_pcm_status *status);
snd_pcm_sframes_t get_snd_pcm_delay(int card, void *handle);
int snd_pcm_hw_sync_ioctl(int card, void *handle);
int prepare_snd_pcm(int card, void *handle);
int reset_snd_pcm(int card, void *handle);
int start_snd_pcm(int card, void *handle);
int drain_snd_pcm(int card, void *handle);
int drop_snd_pcm(int card, void *handle);
int free_snd_pcm(int card, void *handle);
size_t get_snd_pcm_frame_bytes(int card);
snd_pcm_sframes_t snd_pcm_write_frames(int card, void *handle, const void *buf, snd_pcm_uframes_t frames);
bool snd_pcm_poll_ready(int card, void *handle);
