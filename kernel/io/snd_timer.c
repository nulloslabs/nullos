#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <sound/asound.h>
#include <main/string.h>
#include <io/audio.h>
#include <io/devices.h>
#include <io/snd_timer.h>
#include <io/time.h>
#include <mm/mm.h>

static int timer_users = 0;

static uint64_t read_timer_ticks(snd_timer_t *t) {
    if (t->pcm) {
        uint64_t staged = get_audio_staged_frames();
        uint64_t pending = get_audio_pending_frames();
        uint64_t played = staged > pending ? staged - pending : 0;
        if (!t->running || t->paused) return t->accumulated;
        return t->accumulated + (played > t->anchor_frames ? played - t->anchor_frames : 0);
    }
    if (!t->running || t->paused) return t->accumulated;
    return t->accumulated + (get_monotonic_time_us() - t->anchor_us) / 1000;
}

static uint32_t get_timer_resolution(snd_timer_t *t) {
    if (!t->pcm) return SND_TIMER_RESOLUTION_NS;
    uint32_t rate = get_audio_rate();
    if (rate == 0) return SND_TIMER_RESOLUTION_NS;
    return 1000000000u / rate;
}

static void fill_timer_tstamp(struct timespec *ts) {
    uint64_t us = get_monotonic_time_us();
    ts->tv_sec = (time_t)(us / 1000000);
    ts->tv_nsec = (long)((us % 1000000) * 1000);
}

static bool check_timer_id(const struct snd_timer_id *id, bool *pcm) {
    if (!id) return false;
    if (id->dev_class == SNDRV_TIMER_CLASS_GLOBAL && id->device == SNDRV_TIMER_GLOBAL_SYSTEM) {
        if (pcm) *pcm = false;
        return true;
    }
    if (id->dev_class == SNDRV_TIMER_CLASS_PCM && id->card == 0 && id->device == 0 && get_audio_rate() > 0) {
        if (pcm) *pcm = true;
        return true;
    }
    return false;
}

void *open_snd_timer(int index) {
    (void)index;
    snd_timer_t *t = malloc(sizeof(snd_timer_t));
    if (!t) return 0;
    memset(t, 0, sizeof(*t));
    t->period_ticks = SND_TIMER_DEFAULT_TICKS;
    timer_users++;
    return t;
}

void release_snd_timer(void *handle) {
    if (!handle) return;
    free(handle);
    timer_users--;
}

int register_snd_timer(void) {
    int rc = register_device("snd/timer", read_snd_timer, write_snd_timer);
    if (rc < 0) return rc;
    return set_devtmpfs_device_hooks("snd/timer", open_snd_timer, release_snd_timer);
}

uint64_t read_snd_timer(void *buf, uint64_t count, uint64_t offset, int index, void *handle) {
    (void)offset;
    (void)index;
    snd_timer_t *t = handle;
    if (!t || !t->selected_valid) return (uint64_t)-EBADFD;
    size_t esize = t->tread ? sizeof(struct snd_timer_tread) : sizeof(struct snd_timer_read);
    if (count < esize) return (uint64_t)-EINVAL;
    if (!t->running || t->paused) return (uint64_t)-EBADFD;
    uint8_t *out = buf;
    size_t done = 0;
    while (done + esize <= count) {
        uint64_t target = (t->consumed / t->period_ticks + 1) * t->period_ticks;
        for (;;) {
            if (!t->running || t->paused) {
                if (done > 0) return done;
                return (uint64_t)-EBADFD;
            }
            if (read_timer_ticks(t) >= target) break;
            uint64_t ahead = target - read_timer_ticks(t);
            if (t->pcm) ahead = ahead * 1000 / (get_audio_rate() ? get_audio_rate() : 48000);
            if (ahead > 10) ahead = 10;
            sleep_us(ahead * 1000);
            __asm__ volatile ("pause");
        }
        if (t->tread) {
            struct snd_timer_tread *ev = (struct snd_timer_tread *)(out + done);
            memset(ev, 0, sizeof(*ev));
            if (t->pending_start) {
                ev->event = SNDRV_TIMER_EVENT_START;
                ev->val = get_timer_resolution(t);
                t->pending_start = false;
            } else {
                ev->event = SNDRV_TIMER_EVENT_TICK;
                ev->val = t->period_ticks;
            }
            fill_timer_tstamp(&ev->tstamp);
        } else {
            struct snd_timer_read *ev = (struct snd_timer_read *)(out + done);
            ev->resolution = get_timer_resolution(t);
            ev->ticks = t->period_ticks;
        }
        t->consumed = target;
        done += esize;
    }
    return done;
}

uint64_t write_snd_timer(const void *buf, uint64_t count, uint64_t offset, int index, void *handle) {
    (void)buf;
    (void)count;
    (void)offset;
    (void)index;
    (void)handle;
    return (uint64_t)-EPERM;
}

int next_snd_timer(struct snd_timer_id *tid) {
    if (!tid) return -EINVAL;
    if (tid->dev_class == SNDRV_TIMER_CLASS_GLOBAL) {
        if (tid->device < -1) return -EINVAL;
        if (tid->device < 0) { tid->device = SNDRV_TIMER_GLOBAL_SYSTEM; return 0; }
        return -ENOENT;
    }
    if (tid->dev_class == SNDRV_TIMER_CLASS_PCM) {
        if (tid->card < -1 || tid->device < -1 || tid->subdevice < -1) return -EINVAL;
        if (get_audio_rate() == 0) return -ENOENT;
        if (tid->card < 0 || (tid->card == 0 && tid->device < 0) || (tid->card == 0 && tid->device == 0 && tid->subdevice < 0)) {
            tid->card = 0;
            tid->device = 0;
            tid->subdevice = 0;
            return 0;
        }
        return -ENOENT;
    }
    if (tid->dev_class == SNDRV_TIMER_CLASS_CARD) return -ENOENT;
    return -EINVAL;
}

int get_snd_timer_ginfo(struct snd_timer_ginfo *info) {
    if (!info) return -EINVAL;
    memset(info, 0, sizeof(*info));
    info->card = -1;
    strncpy((char *)info->id, "system", sizeof(info->id) - 1);
    strncpy((char *)info->name, "System Timer", sizeof(info->name) - 1);
    info->resolution = SND_TIMER_RESOLUTION_NS;
    info->resolution_min = SND_TIMER_RESOLUTION_NS;
    info->resolution_max = SND_TIMER_RESOLUTION_NS;
    info->clients = (uint32_t)(timer_users < 0 ? 0 : timer_users);
    return 0;
}

int set_snd_timer_gparams(const struct snd_timer_gparams *params) {
    if (!params) return -EINVAL;
    if (params->period_den == 0) return -EINVAL;
    return 0;
}

int get_snd_timer_gstatus(struct snd_timer_gstatus *status) {
    if (!status) return -EINVAL;
    memset(status, 0, sizeof(*status));
    status->resolution = SND_TIMER_RESOLUTION_NS;
    status->resolution_num = 1;
    status->resolution_den = 1000;
    return 0;
}


int select_snd_timer(void *handle, const struct snd_timer_select *sel) {
    snd_timer_t *t = handle;
    if (!t || !sel) return -EINVAL;
    bool pcm = false;
    if (!check_timer_id(&sel->id, &pcm)) return -ENODEV;
    t->pcm = pcm;
    t->selected = sel->id;
    t->selected_valid = true;
    t->running = false;
    t->paused = false;
    t->accumulated = 0;
    t->consumed = 0;
    t->pending_start = false;
    return 0;
}

int get_snd_timer_info(void *handle, struct snd_timer_info *info) {
    snd_timer_t *t = handle;
    if (!t || !info) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    memset(info, 0, sizeof(*info));
    info->card = -1;
    strncpy((char *)info->id, t->pcm ? "pcm" : "system", sizeof(info->id) - 1);
    strncpy((char *)info->name, t->pcm ? "PCM Timer" : "System Timer", sizeof(info->name) - 1);
    info->resolution = get_timer_resolution(t);
    return 0;
}

int set_snd_timer_params(void *handle, const struct snd_timer_params *params) {
    snd_timer_t *t = handle;
    if (!t || !params) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    if (params->ticks == 0 || params->ticks > SND_TIMER_MAX_TICKS) return -EINVAL;
    t->period_ticks = params->ticks;
    if (params->flags & SNDRV_TIMER_PSFLG_AUTO) {
        t->accumulated = 0;
        t->consumed = 0;
        t->anchor_us = get_monotonic_time_us();
        t->anchor_frames = get_audio_staged_frames() - get_audio_pending_frames();
        t->running = true;
        t->paused = false;
        t->pending_start = true;
    }
    return 0;
}

int get_snd_timer_status(void *handle, struct snd_timer_status *status) {
    snd_timer_t *t = handle;
    if (!t || !status) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    memset(status, 0, sizeof(*status));
    fill_timer_tstamp(&status->tstamp);
    status->resolution = get_timer_resolution(t);
    return 0;
}

int start_snd_timer(void *handle) {
    snd_timer_t *t = handle;
    if (!t) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    t->accumulated = 0;
    t->consumed = 0;
    t->anchor_us = get_monotonic_time_us();
    t->anchor_frames = get_audio_staged_frames() - get_audio_pending_frames();
    t->running = true;
    t->paused = false;
    t->pending_start = true;
    return 0;
}

int stop_snd_timer(void *handle) {
    snd_timer_t *t = handle;
    if (!t) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    t->accumulated = read_timer_ticks(t);
    t->running = false;
    t->paused = false;
    return 0;
}

int continue_snd_timer(void *handle) {
    snd_timer_t *t = handle;
    if (!t) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    if (!t->paused) {
        t->accumulated = 0;
        t->consumed = 0;
    }
    t->anchor_us = get_monotonic_time_us();
    t->anchor_frames = get_audio_staged_frames() - get_audio_pending_frames();
    t->running = true;
    t->paused = false;
    t->pending_start = true;
    return 0;
}

int pause_snd_timer(void *handle) {
    snd_timer_t *t = handle;
    if (!t) return -EINVAL;
    if (!t->selected_valid) return -EBADFD;
    if (!t->running) return 0;
    t->accumulated = read_timer_ticks(t);
    t->paused = true;
    return 0;
}

int set_snd_timer_tread(void *handle, int enable) {
    snd_timer_t *t = handle;
    if (!t) return -EINVAL;
    t->tread = enable != 0;
    return 0;
}
