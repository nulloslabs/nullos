#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sound/asound.h>

#define SND_TIMER_RESOLUTION_NS 1000000
#define SND_TIMER_DEFAULT_TICKS 100
#define SND_TIMER_MAX_TICKS     60000

typedef struct {
    bool running;
    bool paused;
    bool tread;
    bool pcm;
    uint32_t period_ticks;
    uint64_t anchor_us;
    uint64_t anchor_frames;
    uint64_t accumulated;
    uint64_t consumed;
    bool pending_start;
    struct snd_timer_id selected;
    bool selected_valid;
} snd_timer_t;

int register_snd_timer(void);
uint64_t read_snd_timer(void *buf, uint64_t count, uint64_t offset, int index, void *handle);
uint64_t write_snd_timer(const void *buf, uint64_t count, uint64_t offset, int index, void *handle);
void *open_snd_timer(int index);
void release_snd_timer(void *handle);
int next_snd_timer(struct snd_timer_id *tid);
int get_snd_timer_ginfo(struct snd_timer_ginfo *info);
int set_snd_timer_gparams(const struct snd_timer_gparams *params);
int get_snd_timer_gstatus(struct snd_timer_gstatus *status);
int select_snd_timer(void *handle, const struct snd_timer_select *sel);
int get_snd_timer_info(void *handle, struct snd_timer_info *info);
int set_snd_timer_params(void *handle, const struct snd_timer_params *params);
int get_snd_timer_status(void *handle, struct snd_timer_status *status);
int start_snd_timer(void *handle);
int stop_snd_timer(void *handle);
int continue_snd_timer(void *handle);
int pause_snd_timer(void *handle);
int set_snd_timer_tread(void *handle, int enable);
