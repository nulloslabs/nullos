#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sound/asound.h>

typedef enum {
    AUDIO_NONE = 0,
    AUDIO_AC97,
} audio_driver_t;

extern audio_driver_t current_audio_driver;

typedef struct {
    uint32_t rate;
    uint8_t channels;
    int format;
    // Stream geometry in caller frames. 0 selects the driver default.
    uint32_t period_frames;
    uint32_t buffer_periods;
} audio_params_t;

typedef struct {
    char name[16];
    char longname[80];
    char mixername[80];
    char components[128];
    char pcm_name[80];
} audio_driver_info_t;

bool is_audio_playing(void);
bool is_audio_started(void);
bool is_audio_muted(void);
int set_audio_volume(uint8_t left, uint8_t right);
void get_audio_volume(uint8_t *left, uint8_t *right);
int mute_audio(bool muted);
int set_audio_params(const audio_params_t *params);
uint32_t get_audio_rate(void);
int get_audio_driver_info(audio_driver_t driver, audio_driver_info_t *info);
uint64_t get_audio_staged_frames(void);
uint64_t get_audio_pending_frames(void);
uint64_t get_audio_ring_bytes(void);
uint64_t get_audio_ring_phys(void);
uint64_t get_audio_ring_frames(void);
uint64_t get_audio_period_frames(void);
uint32_t get_audio_max_period_frames(void);
int set_audio_start_threshold(uint64_t frames);
int commit_audio_frames(uint64_t appl_frames);
int kick_audio(void);
void reset_audio_mmap(void);
int drain_audio(uint64_t appl_frames);
void drop_audio(void);
int play_audio(void *buf, size_t size);
