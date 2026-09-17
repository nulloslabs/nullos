#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <sound/asound.h>
#include <main/log.h>
#include <main/string.h>
#include <main/sched.h>
#include <io/audio.h>
#include <io/devices.h>
#include <io/time.h>
#include <io/snd.h>
#include <io/snd_pcm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>

static snd_pcm_stream_t streams[SND_MAX_CARDS];

static snd_pcm_stream_t *check_snd_pcm(int card) {
    if (card < 0 || card >= SND_MAX_CARDS) return 0;
    const snd_card_t *c = get_snd_card(card);
    if (!c || !c->present || c->driver == AUDIO_NONE) return 0;
    return &streams[card];
}

static size_t get_stream_frame_bytes(const snd_pcm_stream_t *s) {
    size_t sample = s->format == SNDRV_PCM_FORMAT_U8 ? 1 : 2;
    return (size_t)s->channels * sample;
}

static uint64_t get_stream_played(void) {
    uint64_t staged = get_audio_staged_frames();
    uint64_t pending = get_audio_pending_frames();
    return staged > pending ? staged - pending : 0;
}

static uint64_t get_stream_appl(snd_pcm_stream_t *s, snd_pcm_handle_t *h) {
    (void)h;
    return s->appl_frames;
}

static uint64_t get_stream_delay(snd_pcm_stream_t *s, snd_pcm_handle_t *h) {
    uint64_t appl = get_stream_appl(s, h);
    uint64_t played = get_stream_played();
    return appl > played ? appl - played : 0;
}

static uint64_t pcm_boundary(uint64_t buffer_frames) {
    uint64_t boundary = buffer_frames;
    while (boundary * 2 <= 0x7FFFFFFFFFFFFFFFULL - buffer_frames) boundary *= 2;
    return boundary;
}

static uint64_t pcm_handle_boundary(snd_pcm_stream_t *s, snd_pcm_handle_t *h) {
    if (h && h->boundary) return h->boundary;
    return s->boundary;
}

static snd_pcm_state_t pcm_state(snd_pcm_stream_t *s, snd_pcm_handle_t *h) {
    if (s->state == SNDRV_PCM_STATE_RUNNING) {
        uint64_t staged = get_audio_staged_frames();
        uint64_t pending = get_audio_pending_frames();
        uint64_t played = staged > pending ? staged - pending : 0;
        uint64_t appl = s->appl_frames;
        if (played > appl) {
            log("snd pcm: XRUN staged=%lu pending=%lu played=%lu appl=%lu\n",
                staged, pending, played, appl);
            s->state = SNDRV_PCM_STATE_XRUN;
        }
    } else if (s->state == SNDRV_PCM_STATE_PREPARED && s->appl_frames > 0 && is_audio_started()) {
        // The engine auto-starts on the start threshold behind our back:
        // pace-to-realtime can consume most of a big write before the
        // write path rechecks the threshold. Mirror the hardware so
        // STATUS never reports PREPARED with an advancing hw_ptr.
        s->state = SNDRV_PCM_STATE_RUNNING;
    }
    (void)h;
    return s->state;
}

static snd_pcm_sframes_t stage_frames(snd_pcm_stream_t *s, const void *buf, snd_pcm_uframes_t frames) {
    int rc = play_audio((void *)buf, (size_t)frames * get_stream_frame_bytes(s));
    if (rc < 0) return rc;
    s->appl_frames += frames;
    return (snd_pcm_sframes_t)frames;
}

static snd_pcm_sframes_t snd_pcm_write_frames_impl(int card, void *handle, const void *buf, snd_pcm_uframes_t frames) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return -EBADFD;
    snd_pcm_handle_t *h = handle;
    snd_pcm_state_t st = pcm_state(s, h);
    if (st == SNDRV_PCM_STATE_XRUN) return -EPIPE;
    if (st != SNDRV_PCM_STATE_PREPARED && st != SNDRV_PCM_STATE_RUNNING) return -EBADFD;
    if (!buf || frames == 0) return 0;

    const uint8_t *p = buf;
    size_t fb = get_stream_frame_bytes(s);
    snd_pcm_uframes_t done = 0;
    uint64_t stall_start = get_monotonic_time_us();
    while (done < frames) {
        uint64_t delay = get_stream_delay(s, h);
        if (s->state == SNDRV_PCM_STATE_RUNNING && get_stream_played() > s->appl_frames) {
            s->state = SNDRV_PCM_STATE_XRUN;
            break;
        }
        uint64_t avail = delay >= s->buffer_frames ? 0 : s->buffer_frames - delay;
        if (avail == 0) {
            // Buffer full: block like Linux write(), bounded for a stalled
            // engine. Sleep instead of pause-spinning: each delay check
            // does PIO (CIV) + HPET MMIO reads (VM exits under KVM);
            // tight spinning caused periodic ~1s audio gaps.
            if (get_monotonic_time_us() - stall_start > PCM_STALL_TIMEOUT_US) {
                return done > 0 ? (snd_pcm_sframes_t)done : -ETIMEDOUT;
            }
            let_current_task_sleep(1000);
            continue;
        }
        stall_start = get_monotonic_time_us();
        snd_pcm_uframes_t n = frames - done;
        if (n > avail) n = (snd_pcm_uframes_t)avail;
        snd_pcm_sframes_t w = stage_frames(s, p + done * fb, n);
        if (w < 0) return done > 0 ? (snd_pcm_sframes_t)done : w;
        if (w == 0) break;
        done += (snd_pcm_uframes_t)w;
        if (s->state == SNDRV_PCM_STATE_PREPARED) {
            uint64_t threshold = h ? h->start_threshold : SND_PCM_DEFAULT_START_THRESHOLD;
            if (get_stream_delay(s, h) >= threshold) {
                int rc = kick_audio();
                if (rc < 0 && rc != -EBUSY) return done > 0 ? (snd_pcm_sframes_t)done : rc;
                s->state = SNDRV_PCM_STATE_RUNNING;
            }
        }
    }
    if (done > 0) pcm_state(s, h); // post-transfer underrun check
    if (done == 0 && s->state == SNDRV_PCM_STATE_XRUN) return -EPIPE;
    return (snd_pcm_sframes_t)done;
}

static void fill_pcm_mmap_status(snd_pcm_handle_t *h, struct snd_pcm_mmap_status *st) {
    snd_pcm_stream_t *s = h ? check_snd_pcm(h->card) : 0;
    memset(st, 0, sizeof(*st));
    if (!s || !s->configured) {
        st->state = SNDRV_PCM_STATE_OPEN;
        return;
    }
    st->state = pcm_state(s, h);
    uint64_t boundary = pcm_handle_boundary(s, h);
    st->hw_ptr = get_stream_played() % boundary;
    uint64_t us = get_monotonic_time_us();
    st->tstamp.tv_sec = (time_t)(us / 1000000);
    st->tstamp.tv_nsec = (long)((us % 1000000) * 1000);
}

static int reconcile_appl(snd_pcm_stream_t *s, snd_pcm_handle_t *h) {
    struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
    uint64_t boundary = pcm_handle_boundary(s, h);
    uint64_t page = ct->appl_ptr % boundary;
    uint64_t cur = s->appl_frames % boundary;
    if (page == cur) return 0;
    uint64_t fwd = (page + boundary - cur) % boundary;
    if (fwd > boundary / 2) {
        // Rewind: pull the producer position back; exposed periods stay.
        uint64_t back = boundary - fwd;
        s->appl_frames = s->appl_frames >= back ? s->appl_frames - back : 0;
    } else {
        uint64_t delay = get_stream_delay(s, h);
        if (delay + fwd > s->buffer_frames) {
            // Overrun of the negotiated buffer: underrun equivalent.
            drop_audio();
            s->appl_frames = get_stream_played();
            s->state = SNDRV_PCM_STATE_XRUN;
            fill_pcm_mmap_status(h, phys_to_virt(h->status_phys));
            ct->appl_ptr = s->appl_frames % boundary;
            return -EPIPE;
        }
        s->appl_frames += fwd;
        commit_audio_frames(s->appl_frames);
        // No auto-start here: like the kernel's sync_ptr, only the write
        // path triggers threshold starts; mmap clients START explicitly.
    }
    ct->appl_ptr = s->appl_frames % boundary;
    return 0;
}

static bool refine_param_mask(struct snd_mask *mask, uint32_t hw, bool *changed) {
    if (!mask) return false;
    // Linux masks are 256 bits wide and fully-open requests (snd_pcm_any)
    // set every word. The capability space fits in word 0, so intersect
    // every word against it and report empty only when nothing survives.
    uint32_t want = mask->bits[0];
    if (want == 0) return false;
    uint32_t got = want & hw;
    if (got == 0) return false;
    bool had_upper = false;
    for (int w = 1; w < 8; w++) {
        if (mask->bits[w] != 0) { had_upper = true; break; }
    }
    if (got != want || had_upper) {
        mask->bits[0] = got;
        memset(&mask->bits[1], 0, sizeof(mask->bits) - sizeof(uint32_t));
        *changed = true;
    }
    return true;
}

static bool refine_param_interval(struct snd_interval *iv, uint32_t lo, uint32_t hi, bool *changed) {
    if (!iv) return false;
    uint32_t old_min = iv->min;
    uint32_t old_max = iv->max;
    bool old_openmin = iv->openmin != 0;
    bool old_openmax = iv->openmax != 0;
    uint32_t nmin = iv->min < lo ? lo : iv->min;
    bool nopenmin = nmin == iv->min ? old_openmin : false;
    uint32_t nmax = iv->max > hi ? hi : iv->max;
    bool nopenmax = nmax == iv->max ? old_openmax : false;
    // Effective integer range: (11 12) => [12,11] is empty for integers.
    // A single open value like (11 11) is also empty.
    uint64_t eff_min = (uint64_t)nmin + (nopenmin ? 1 : 0);
    uint64_t eff_max;
    if (nopenmax) {
        if (nmax == 0) { iv->empty = 1; return false; }
        eff_max = (uint64_t)nmax - 1;
    } else {
        eff_max = nmax;
    }
    if (eff_min > eff_max) { iv->empty = 1; return false; }
    iv->min = nmin;
    iv->max = nmax;
    iv->openmin = nopenmin ? 1 : 0;
    iv->openmax = nopenmax ? 1 : 0;
    iv->integer = 1;
    iv->empty = 0;
    if (iv->min != old_min || iv->max != old_max || (iv->openmin != 0) != old_openmin || (iv->openmax != 0) != old_openmax) *changed = true;
    return true;
}

static uint32_t choose_interval_min(const struct snd_interval *iv) {
    uint32_t v = iv->min;
    if (iv->openmin) v++;
    return v;
}

static uint32_t choose_interval_max(const struct snd_interval *iv) {
    uint32_t v = iv->max;
    if (iv->openmax) v--;
    return v;
}
static int snd_pcm_hw_sync(snd_pcm_stream_t *s, snd_pcm_handle_t *h, snd_pcm_sframes_t *delay_out) {
    if (!s || !s->configured) return -EBADFD;
    snd_pcm_state_t st = pcm_state(s, h);
    if (st == SNDRV_PCM_STATE_XRUN) return -EPIPE;
    if (st != SNDRV_PCM_STATE_RUNNING && st != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    if (delay_out) *delay_out = (snd_pcm_sframes_t)get_stream_delay(s, h);
    return 0;
}

static int choose_geometry(struct snd_pcm_hw_params *params, bool mmap_only, uint32_t *period_out, uint32_t *periods_out, uint32_t *buffer_out) {
    struct snd_interval *piv = &params->intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - 8];
    struct snd_interval *niv = &params->intervals[SNDRV_PCM_HW_PARAM_PERIODS - 8];
    struct snd_interval *biv = &params->intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - 8];
    uint32_t period_max = get_audio_max_period_frames();
    if (period_max == 0) period_max = PCM_PERIOD_MAX_FRAMES;
    uint32_t capacity = PCM_MAX_PERIODS * period_max;

    if (mmap_only) {
        // The DMA ring is the buffer: only its exact geometry commits.
        *period_out = period_max;
        *periods_out = PCM_MAX_PERIODS;
        *buffer_out = capacity;
        return 0;
    }

    uint32_t pmin = choose_interval_min(piv);
    uint32_t pmax = choose_interval_max(piv);
    uint32_t nmin = choose_interval_min(niv);
    uint32_t nmax = choose_interval_max(niv);
    uint32_t bmin = choose_interval_min(biv);
    uint32_t bmax = choose_interval_max(biv);
    if (pmin < 1) pmin = 1;
    if (nmin < 1) nmin = 1;
    if (nmax > PCM_MAX_PERIODS) nmax = PCM_MAX_PERIODS;
    if (bmax > capacity) bmax = capacity;
    if (pmin < PCM_PERIOD_MIN_FRAMES) pmin = PCM_PERIOD_MIN_FRAMES;
    // Open-empty intervals like PERIODS (11 12) have no integer.
    // Widen them so the search below can pick the nearest compatible
    // triple instead of failing `aplay` with -EINVAL.
    // This is install-time tolerance: HW_REFINE still fails for empty
    // so alsa-lib _near can backtrack; HW_PARAMS coerces and succeeds.
    if (pmin > pmax) { pmin = PCM_PERIOD_MIN_FRAMES; pmax = period_max; }
    if (nmin > nmax) { nmin = 1; nmax = PCM_MAX_PERIODS; }
    if (bmin > bmax) { bmin = PCM_PERIOD_MIN_FRAMES; bmax = capacity; }
    if (pmax > period_max) pmax = period_max;
    if (bmin < PCM_PERIOD_MIN_FRAMES) bmin = PCM_PERIOD_MIN_FRAMES;
    if (bmax < bmin || pmax < pmin || nmax < nmin) return -EINVAL;

    // ALSA expects buffer == period * periods. Search exact triples
    // first (largest buffer, then largest period for stability).
    for (uint32_t p = pmax; p >= pmin; p--) {
        uint32_t n_lo = (uint32_t)((bmin + p - 1) / p); // ceil
        uint32_t n_hi = (uint32_t)(bmax / p);           // floor
        if (n_lo < nmin) n_lo = nmin;
        if (n_hi > nmax) n_hi = nmax;
        if (n_lo > n_hi) {
            if (p == pmin) break;
            continue;
        }
        // Largest n => largest buffer for this period.
        uint32_t n = n_hi;
        uint32_t b = p * n;
        *period_out = p;
        *periods_out = n;
        *buffer_out = b;
        return 0;
    }

    // No exact triple in range (e.g. buffer 24000 single with period
    // 2046 single: 24000 % 2046 != 0). Keep the requested buffer and
    // pick the nearest divisor period for it. This may step outside
    // [pmin,pmax] by a few frames (2000 vs 2046) but makes plain
    // `aplay file.wav` work instead of failing with PERIODS (11 12).
    {
        uint32_t target = bmax;
        uint32_t best_p = 0, best_n = 0;
        uint32_t best_dist = 0xFFFFFFFFU;
        uint32_t slot_lo = (target + PCM_MAX_PERIODS - 1) / PCM_MAX_PERIODS;
        if (slot_lo < 1) slot_lo = 1;
        uint32_t slot_hi = period_max < target ? period_max : target;
        for (uint32_t p = slot_hi; p >= slot_lo; p--) {
            if (target % p != 0) {
                if (p == slot_lo) break;
                continue;
            }
            uint32_t n = target / p;
            if (n < 1 || n > PCM_MAX_PERIODS) {
                if (p == slot_lo) break;
                continue;
            }
            uint32_t dist = 0;
            if (p < pmin) dist = pmin - p;
            else if (p > pmax) dist = p - pmax;
            // Prefer closer to requested period, then larger period.
            if (dist < best_dist || (dist == best_dist && p > best_p)) {
                best_dist = dist;
                best_p = p;
                best_n = n;
                if (dist == 0) break; // cannot do better below? keep largest with dist 0? first found descending is largest, good.
                // Since we descend, first dist-0 found is the largest feasible inside range.
                if (dist == 0) break;
            }
            if (p == slot_lo) break;
        }
        if (best_p) {
            *period_out = best_p;
            *periods_out = best_n;
            *buffer_out = target;
            return 0;
        }
    }

    // Prime-ish buffer with no suitable divisor (e.g. 24001): keep the
    // requested period and adjust the buffer to the nearest multiple.
    {
        uint32_t best_b = 0, best_p = 0, best_n = 0;
        uint32_t best_dist = 0xFFFFFFFFU;
        for (uint32_t p = pmax; p >= pmin; p--) {
            for (uint32_t n = nmin; n <= nmax; n++) {
                uint64_t b = (uint64_t)p * n;
                if (b < PCM_PERIOD_MIN_FRAMES || b > capacity) continue;
                uint32_t dist = 0;
                if (b < bmin) dist = (uint32_t)(bmin - b);
                else if (b > bmax) dist = (uint32_t)(b - bmax);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_b = (uint32_t)b;
                    best_p = p;
                    best_n = n;
                    if (dist == 0) break;
                }
            }
            if (best_dist == 0) break;
            if (p == pmin) break;
        }
        if (best_b) {
            *period_out = best_p;
            *periods_out = best_n;
            *buffer_out = best_b;
            return 0;
        }
    }

    return -EINVAL;
}

int set_snd_pcm_params(int card, void *handle, struct snd_pcm_hw_params *params) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !params) return -ENXIO;
    snd_pcm_handle_t *h = handle;
    snd_pcm_state_t st = pcm_state(s, h);
    // Kernel rules: hw_params from OPEN/SETUP/PREPARED (no live mmaps).
    if (st != SNDRV_PCM_STATE_OPEN && st != SNDRV_PCM_STATE_SETUP && st != SNDRV_PCM_STATE_PREPARED) return -EBADFD;

    // Install-time tolerance: alsa-lib/plug may send open-empty intervals
    // like PERIODS (11 12) (no integer >11 and <12) when buffer 24000 and
    // period 2046 do not divide evenly. HW_REFINE correctly rejects those
    // so _near can backtrack, but HW_PARAMS must coerce and succeed so
    // plain `aplay file.wav` just works. Clear openness to make them
    // closed ([11,12]) before refining; geometry below then picks the
    // nearest compatible triple (e.g. 2000x12).
    for (int i = 0; i < 12; i++) {
        params->intervals[i].openmin = 0;
        params->intervals[i].openmax = 0;
        params->intervals[i].empty = 0;
    }
    params->rmask = ~0U;
    const char *stage = "refine";
    int rc = refine_snd_pcm(card, params);
    if (rc < 0) goto error;

    uint32_t access = 0;
    uint32_t format = 0;
    uint32_t access_bits = params->masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0];
    for (uint32_t i = 0; i < 32; i++) {
        if (access_bits & (1u << i)) { access = i; break; }
    }
    uint32_t format_bits = params->masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0];
    for (uint32_t i = 0; i < 32; i++) {
        if (format_bits & (1u << i)) { format = i; break; }
    }
    if (!(params->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT].bits[0] & (1u << SNDRV_PCM_SUBFORMAT_STD))) { rc = -EINVAL; goto error; }

    bool mmap_only = access == SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
    uint32_t channels = choose_interval_min(&params->intervals[SNDRV_PCM_HW_PARAM_CHANNELS - 8]);
    uint32_t rate = choose_interval_min(&params->intervals[SNDRV_PCM_HW_PARAM_RATE - 8]);
    uint32_t period = 0, periods = 0, buffer = 0;
    stage = "geometry";
    rc = choose_geometry(params, mmap_only, &period, &periods, &buffer);
    if (rc < 0) goto error;

    stage = "validate";
    if (channels < 1 || channels > 2 || rate < SND_PCM_HW_RATE_MIN || rate > SND_PCM_HW_RATE_MAX) { rc = -EINVAL; goto error; }
    if (format != SNDRV_PCM_FORMAT_U8 && format != SNDRV_PCM_FORMAT_S16_LE) { rc = -EINVAL; goto error; }
    if (mmap_only && (format != SNDRV_PCM_FORMAT_S16_LE || channels != 2)) { rc = -EINVAL; goto error; }

    // The DMA slot underneath must divide the buffer so a ring slot never
    // wraps mid-slot (the engine exposes one contiguous region per slot).
    // It is decoupled from the ALSA period above, which is whatever the
    // app negotiated.
    uint32_t wire = get_audio_max_period_frames();
    uint32_t slot = 0;
    uint32_t smax = wire < buffer ? wire : buffer;
    for (uint32_t s = smax; s >= 1; s--) {
        if (buffer % s != 0) continue;
        if (buffer / s > PCM_MAX_PERIODS) continue;
        slot = s;
        break;
    }
    if (slot == 0) { rc = -EINVAL; goto error; }

    stage = "backend";
    audio_params_t ap;
    ap.rate = rate;
    ap.channels = (uint8_t)channels;
    ap.format = (int)format;
    ap.period_frames = slot;
    ap.buffer_periods = buffer / slot;
    rc = set_audio_params(&ap);
    if (rc < 0) goto error;

    s->rate = ap.rate;
    s->channels = ap.channels;
    s->format = ap.format;
    s->configured = true;
    s->appl_frames = 0;
    s->buffer_frames = buffer;
    s->boundary = pcm_boundary(buffer);
    s->state = SNDRV_PCM_STATE_SETUP;

    uint32_t frame_bytes = channels * (format == SNDRV_PCM_FORMAT_U8 ? 1 : 2);
    params->info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_SYNC_APPLPTR | SNDRV_PCM_INFO_PERFECT_DRAIN | SNDRV_PCM_INFO_INTERLEAVED;
    params->msbits = format == SNDRV_PCM_FORMAT_U8 ? 8 : 16;
    params->rate_num = ap.rate;
    params->rate_den = 1;
    params->masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0] = 1u << access;
    params->masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0] = 1u << format;
    params->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT].bits[0] = 1u << SNDRV_PCM_SUBFORMAT_STD;
    uint32_t single[][2] = {
        { SNDRV_PCM_HW_PARAM_SAMPLE_BITS, params->msbits },
        { SNDRV_PCM_HW_PARAM_FRAME_BITS, params->msbits * channels },
        { SNDRV_PCM_HW_PARAM_CHANNELS, channels },
        { SNDRV_PCM_HW_PARAM_RATE, rate },
        { SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period },
        { SNDRV_PCM_HW_PARAM_PERIODS, periods },
        { SNDRV_PCM_HW_PARAM_BUFFER_SIZE, buffer },
        { SNDRV_PCM_HW_PARAM_PERIOD_BYTES, period * frame_bytes },
        { SNDRV_PCM_HW_PARAM_BUFFER_BYTES, buffer * frame_bytes },
        { SNDRV_PCM_HW_PARAM_PERIOD_TIME, (uint32_t)(period * 1000000ULL / rate) },
        { SNDRV_PCM_HW_PARAM_BUFFER_TIME, (uint32_t)(buffer * 1000000ULL / rate) },
        { SNDRV_PCM_HW_PARAM_TICK_TIME, 0 },
    };
    for (uint32_t i = 0; i < 12; i++) {
        struct snd_interval *iv = &params->intervals[single[i][0] - 8];
        iv->min = single[i][1];
        iv->max = single[i][1];
        iv->openmin = 0;
        iv->openmax = 0;
        iv->integer = 1;
        iv->empty = 0;
    }

    if (h) {
        h->mmap_mode = mmap_only;
        h->appl_frames = 0;
        // Kernel defaults after hw_params.
        h->avail_min = period;
        h->start_threshold = 1;
        h->stop_threshold = buffer;
        h->boundary = 0; // until sw_params; stream default applies
        struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
        ct->appl_ptr = 0;
        ct->avail_min = h->avail_min;
        set_audio_start_threshold(h->start_threshold);
    }
    return 0;

error:
    log("snd pcm: hw_params failed at %s (rc %d)\n", stage, rc);
    // Like the kernel: a failed hw_params leaves the stream unconfigured.
    if (s->state != SNDRV_PCM_STATE_OPEN) {
        drop_audio();
        s->configured = false;
        s->appl_frames = 0;
        s->buffer_frames = 0;
        s->state = SNDRV_PCM_STATE_OPEN;
        if (h) h->mmap_mode = false;
    }
    return rc;
}

int get_snd_pcm_info(int card, struct snd_pcm_info *info) {
    const snd_card_t *c = get_snd_card(card);
    if (!check_snd_pcm(card) || !info) return -ENXIO;
    memset(info, 0, sizeof(*info));
    info->device = 0;
    info->subdevice = 0;
    info->stream = SNDRV_PCM_STREAM_PLAYBACK;
    info->card = card;
    strncpy((char *)info->id, c->pcm_name, sizeof(info->id) - 1);
    strncpy((char *)info->name, c->pcm_name, sizeof(info->name) - 1);
    strncpy((char *)info->subname, "subdevice #0", sizeof(info->subname) - 1);
    info->subdevices_count = 1;
    info->subdevices_avail = 1;
    return 0;
}

int get_snd_pcm_status(int card, void *handle, struct snd_pcm_status *status) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !status) return -ENXIO;
    snd_pcm_handle_t *h = handle;
    memset(status, 0, sizeof(*status));
    if (!s->configured) {
        status->state = SNDRV_PCM_STATE_OPEN;
        return 0;
    }
    status->state = pcm_state(s, h);
    uint64_t boundary = pcm_handle_boundary(s, h);
    uint64_t appl = get_stream_appl(s, h);
    uint64_t played = get_stream_played();
    // Signed producer offset, like the kernel's playback avail/delay:
    // delay is max(appl-hw, 0) and avail is buffer-(appl-hw), which may
    // exceed the buffer after drain overshoot. (Magnitudes stay tiny next
    // to 2^63, so the casts below are exact.)
    snd_pcm_sframes_t off = appl >= played ? (snd_pcm_sframes_t)(appl - played)
                                           : -(snd_pcm_sframes_t)(played - appl);
    snd_pcm_sframes_t want_avail = (snd_pcm_sframes_t)s->buffer_frames - off;
    status->appl_ptr = appl % boundary;
    status->hw_ptr = played % boundary;
    status->delay = off > 0 ? off : 0;
    status->avail = want_avail > 0 ? (snd_pcm_uframes_t)want_avail : 0;
    uint64_t us = get_monotonic_time_us();
    status->tstamp.tv_sec = (time_t)(us / 1000000);
    status->tstamp.tv_nsec = (long)((us % 1000000) * 1000);
    return 0;
}


snd_pcm_sframes_t get_snd_pcm_delay(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s) return -ENXIO;
    snd_pcm_sframes_t delay = 0;
    int rc = snd_pcm_hw_sync(s, handle, &delay);
    if (rc < 0) return rc;
    return delay;
}

int snd_pcm_hw_sync_ioctl(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s) return -ENXIO;
    return snd_pcm_hw_sync(s, handle, 0);
}

int prepare_snd_pcm(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return -EBADFD;
    snd_pcm_handle_t *h = handle;
    snd_pcm_state_t st = pcm_state(s, h);
    // Kernel rules: EBADFD from OPEN, EBUSY while RUNNING/DRAINING.
    if (st == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (st == SNDRV_PCM_STATE_RUNNING) return -EBUSY;
    if (h && h->mmap_mode) reset_audio_mmap();
    else drop_audio();
    s->appl_frames = 0;
    s->state = SNDRV_PCM_STATE_PREPARED;
    if (h) {
        h->appl_frames = 0;
        struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
        ct->appl_ptr = 0;
        fill_pcm_mmap_status(h, phys_to_virt(h->status_phys));
    }
    return 0;
}

int reset_snd_pcm(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return -EBADFD;
    snd_pcm_handle_t *h = handle;
    snd_pcm_state_t st = pcm_state(s, h);
    // Kernel rules: only RUNNING/PREPARED (and PAUSED/SUSPENDED, which we
    // do not implement); state is kept.
    if (st != SNDRV_PCM_STATE_RUNNING && st != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    if (h && h->mmap_mode) reset_audio_mmap();
    else drop_audio();
    s->appl_frames = 0;
    if (h) {
        h->appl_frames = 0;
        struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
        ct->appl_ptr = 0;
        fill_pcm_mmap_status(h, phys_to_virt(h->status_phys));
    }
    return 0;
}

int start_snd_pcm(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return -EBADFD;
    snd_pcm_handle_t *h = handle;
    if (s->state != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    // snd_pcm_pre_start: an empty playback buffer is -EPIPE.
    if (get_stream_delay(s, h) == 0) return -EPIPE;
    int rc = kick_audio();
    if (rc < 0) return rc;
    s->state = SNDRV_PCM_STATE_RUNNING;
    return 0;
}

int drain_snd_pcm(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return -EBADFD;
    snd_pcm_handle_t *h = handle;
    snd_pcm_state_t st = pcm_state(s, h);
    switch (st) {
        case SNDRV_PCM_STATE_OPEN:
            return -EBADFD;
        case SNDRV_PCM_STATE_XRUN:
        case SNDRV_PCM_STATE_SETUP:
            s->state = SNDRV_PCM_STATE_SETUP;
            return 0;
        case SNDRV_PCM_STATE_PREPARED:
            if (get_stream_delay(s, h) == 0) {
                s->state = SNDRV_PCM_STATE_SETUP;
                return 0;
            }
            s->state = SNDRV_PCM_STATE_DRAINING;
            break;
        case SNDRV_PCM_STATE_RUNNING:
            s->state = SNDRV_PCM_STATE_DRAINING;
            break;
        default:
            return -EBADFD;
    }
    int rc = drain_audio(s->appl_frames);
    if (rc < 0) {
        s->state = SNDRV_PCM_STATE_SETUP;
        return rc;
    }
    s->state = SNDRV_PCM_STATE_SETUP;
    return 0;
}

int drop_snd_pcm(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s) return -ENXIO;
    if (!s->configured || s->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    snd_pcm_handle_t *h = handle;
    // Like Linux, DROP keeps the hardware configuration; the stream lands
    // in SETUP.
    drop_audio();
    s->appl_frames = 0;
    s->state = SNDRV_PCM_STATE_SETUP;
    if (h) {
        h->appl_frames = 0;
        struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
        ct->appl_ptr = 0;
        fill_pcm_mmap_status(h, phys_to_virt(h->status_phys));
    }
    return 0;
}

int free_snd_pcm(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s) return -ENXIO;
    snd_pcm_handle_t *h = handle;
    snd_pcm_state_t st = pcm_state(s, h);
    // Kernel rules: hw_free only from SETUP/PREPARED, back to OPEN.
    if (st != SNDRV_PCM_STATE_SETUP && st != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    drop_audio();
    s->configured = false;
    s->appl_frames = 0;
    s->buffer_frames = 0;
    s->state = SNDRV_PCM_STATE_OPEN;
    if (h) {
        h->mmap_mode = false;
        h->appl_frames = 0;
        struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
        ct->appl_ptr = 0;
        fill_pcm_mmap_status(h, phys_to_virt(h->status_phys));
    }
    return 0;
}

size_t get_snd_pcm_frame_bytes(int card) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return 0;
    return get_stream_frame_bytes(s);
}

int register_snd_pcm(int card, int device) {
    if (card < 0 || card > 9 || device != 0) return -EINVAL;
    char name[] = "snd/pcmC0D0p";
    name[8] = (char)('0' + card);
    int rc = register_device_idx(name, read_snd_pcm, write_snd_pcm, card);
    if (rc < 0) return rc;
    return set_devtmpfs_device_hooks(name, open_snd_pcm, release_snd_pcm);
}

void *open_snd_pcm(int index) {
    if (!check_snd_pcm(index)) return 0;
    snd_pcm_handle_t *h = malloc(sizeof(snd_pcm_handle_t));
    if (!h) return 0;
    uint64_t status = (uint64_t)pmalloc();
    if (!status) { free(h); return 0; }
    uint64_t control = (uint64_t)pmalloc();
    if (!control) { pfree((void *)status); free(h); return 0; }
    memset(h, 0, sizeof(*h));
    h->card = index;
    h->avail_min = SND_PCM_DEFAULT_AVAIL_MIN;
    h->start_threshold = SND_PCM_DEFAULT_START_THRESHOLD;
    h->status_phys = status;
    h->control_phys = control;
    struct snd_pcm_mmap_status *st = phys_to_virt(status);
    struct snd_pcm_mmap_control *ct = phys_to_virt(control);
    memset(st, 0, PAGE_SIZE);
    memset(ct, 0, PAGE_SIZE);
    st->state = SNDRV_PCM_STATE_OPEN;
    ct->avail_min = h->avail_min;
    return h;
}

void release_snd_pcm(void *handle) {
    snd_pcm_handle_t *h = handle;
    if (!h) return;
    if (h->status_phys) pfree((void *)h->status_phys);
    if (h->control_phys) pfree((void *)h->control_phys);
    free(h);
}

bool query_snd_pcm_mmap(const char *rel, void *handle, uint64_t offset, uint64_t *phys_out, uint64_t *pages_out) {
    if (!rel || !phys_out || !pages_out) return false;
    if (strncmp(rel, "snd/", 4) == 0) rel += 4;
    if (rel[0] != 'p' || rel[1] != 'c' || rel[2] != 'm' || rel[3] != 'C') return false;
    if (rel[4] < '0' || rel[4] > '9' || rel[5] != 'D' || rel[6] != '0' || rel[7] != 'p' || rel[8] != '\0') return false;
    snd_pcm_stream_t *s = check_snd_pcm(rel[4] - '0');
    if (!s) return false;
    uint64_t ring_bytes = get_audio_ring_bytes();
    uint64_t ring_phys = get_audio_ring_phys();
    if (s->configured && ring_bytes > 0 && ring_phys != 0 && offset < ring_bytes) {
        *phys_out = ring_phys + offset;
        *pages_out = (ring_bytes - offset + PAGE_SIZE - 1) / PAGE_SIZE;
        return true;
    }
    snd_pcm_handle_t *h = handle;
    if (!h || h->card != rel[4] - '0') return false;
    if (offset == SNDRV_PCM_MMAP_OFFSET_STATUS || offset == SNDRV_PCM_MMAP_OFFSET_STATUS_OLD) {
        *phys_out = h->status_phys;
        *pages_out = 1;
        return true;
    }
    if (offset == SNDRV_PCM_MMAP_OFFSET_CONTROL || offset == SNDRV_PCM_MMAP_OFFSET_CONTROL_OLD) {
        *phys_out = h->control_phys;
        *pages_out = 1;
        return true;
    }
    return false;
}

uint64_t read_snd_pcm(void *buf, uint64_t count, uint64_t offset, int index, void *handle) {
    (void)handle;
    (void)buf;
    (void)count;
    (void)offset;
    (void)index;
    return (uint64_t)-EBADFD;
}

uint64_t write_snd_pcm(const void *buf, uint64_t count, uint64_t offset, int index, void *handle) {
    (void)offset;
    if (!buf) return (uint64_t)-EINVAL;
    snd_pcm_stream_t *s = check_snd_pcm(index);
    if (!s || !s->configured) return (uint64_t)-EBADFD;
    size_t fb = get_stream_frame_bytes(s);
    if (count % fb != 0) return (uint64_t)-EINVAL;
    snd_pcm_sframes_t done = snd_pcm_write_frames(index, handle, buf, count / fb);
    if (done < 0) return (uint64_t)done;
    return (uint64_t)done * fb;
}

snd_pcm_sframes_t snd_pcm_write_frames(int card, void *handle, const void *buf, snd_pcm_uframes_t frames) {
    if (!buf) return -EINVAL;
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return -EBADFD;
    return snd_pcm_write_frames_impl(card, handle, buf, frames);
}

// POLLOUT readiness for poll(): room for at least avail_min frames, like
// Linux. XRUN also reports ready so the client can wake and recover.
bool snd_pcm_poll_ready(int card, void *handle) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !s->configured) return true;
    snd_pcm_state_t st = pcm_state(s, handle);
    if (st == SNDRV_PCM_STATE_XRUN) return true;
    if (st != SNDRV_PCM_STATE_RUNNING && st != SNDRV_PCM_STATE_PREPARED) return true;
    uint64_t played = get_stream_played();
    uint64_t appl = s->appl_frames;
    uint64_t avail;
    if (appl > played) {
        uint64_t off = appl - played;
        avail = off >= s->buffer_frames ? 0 : s->buffer_frames - off;
    } else {
        avail = s->buffer_frames;
    }
    snd_pcm_handle_t *h = handle;
    uint64_t min = h && h->avail_min ? h->avail_min : SND_PCM_DEFAULT_AVAIL_MIN;
    return avail >= min || avail >= s->buffer_frames;
}

int set_snd_pcm_sw(void *handle, struct snd_pcm_sw_params *params) {
    snd_pcm_handle_t *h = handle;
    if (!h || !params) return -EINVAL;
    snd_pcm_stream_t *s = check_snd_pcm(h->card);
    if (!s || !s->configured) return -EBADFD;
    // Mirrors Linux snd_pcm_sw_params: the requested boundary is ignored,
    // the kernel default is always stored and reported back.
    if (params->tstamp_mode < 0 || params->tstamp_mode > SNDRV_PCM_TSTAMP_LAST) return -EINVAL;
    if (params->avail_min == 0) return -EINVAL;
    if (params->silence_size >= (snd_pcm_uframes_t)s->boundary) {
        if (params->silence_threshold != 0) return -EINVAL;
    } else {
        if (params->silence_size > params->silence_threshold) return -EINVAL;
        if (params->silence_threshold > s->buffer_frames) return -EINVAL;
    }
    h->boundary = s->boundary;
    h->avail_min = params->avail_min;
    h->start_threshold = params->start_threshold;
    h->stop_threshold = params->stop_threshold;
    params->boundary = s->boundary; // report the effective value
    struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);
    ct->avail_min = h->avail_min;
    set_audio_start_threshold(h->start_threshold);
    return 0;
}

int sync_snd_pcm(void *handle, struct snd_pcm_sync_ptr *sync) {
    snd_pcm_handle_t *h = handle;
    if (!h || !sync) return -EINVAL;
    snd_pcm_stream_t *s = check_snd_pcm(h->card);
    if (!s || !s->configured) return -EBADFD;
    struct snd_pcm_mmap_status *page = phys_to_virt(h->status_phys);
    struct snd_pcm_mmap_control *ct = phys_to_virt(h->control_phys);

    if (sync->flags & SNDRV_PCM_SYNC_PTR_HWSYNC) {
        snd_pcm_state_t st = pcm_state(s, h);
        if (st == SNDRV_PCM_STATE_XRUN) return -EPIPE;
        if (st != SNDRV_PCM_STATE_RUNNING && st != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    }
    if (h->mmap_mode) {
        if (!(sync->flags & SNDRV_PCM_SYNC_PTR_APPL)) {
            struct snd_pcm_mmap_control *ct_page = phys_to_virt(h->control_phys);
            ct_page->appl_ptr = sync->c.control.appl_ptr;
        }
        int rc = reconcile_appl(s, h);
        if (rc < 0) return rc;
    }
    if (!(sync->flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN)) {
        h->avail_min = sync->c.control.avail_min;
        ct->avail_min = h->avail_min;
    } else {
        sync->c.control.avail_min = h->avail_min;
    }
    fill_pcm_mmap_status(h, page);
    fill_pcm_mmap_status(h, &sync->s.status);
    sync->c.control.appl_ptr = ct->appl_ptr;
    return 0;
}

int refine_snd_pcm(int card, struct snd_pcm_hw_params *params) {
    snd_pcm_stream_t *s = check_snd_pcm(card);
    if (!s || !params) return -ENXIO;
    uint32_t period_max = get_audio_max_period_frames();
    bool changed = false;
    if (!refine_param_mask(&params->masks[SNDRV_PCM_HW_PARAM_ACCESS], SND_PCM_HW_ACCESS_MASK, &changed)) return -EINVAL;
    if (changed) params->cmask |= 1u << SNDRV_PCM_HW_PARAM_ACCESS;
    bool mmap_only = params->masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0] == (1u << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);

    uint32_t formats = SND_PCM_HW_FORMAT_MASK;
    if (mmap_only) formats = 1u << SNDRV_PCM_FORMAT_S16_LE;
    changed = false;
    if (!refine_param_mask(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT], formats, &changed)) return -EINVAL;
    if (changed) params->cmask |= 1u << SNDRV_PCM_HW_PARAM_FORMAT;

    changed = false;
    if (!refine_param_mask(&params->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT], 1u << SNDRV_PCM_SUBFORMAT_STD, &changed)) return -EINVAL;
    if (changed) params->cmask |= 1u << SNDRV_PCM_HW_PARAM_SUBFORMAT;

    uint32_t channel_lo = mmap_only ? 2 : 1;
    uint32_t ivs[][3] = {
        { SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 8, 16 },
        { SNDRV_PCM_HW_PARAM_FRAME_BITS, 8, 32 },
        { SNDRV_PCM_HW_PARAM_CHANNELS, channel_lo, 2 },
        { SNDRV_PCM_HW_PARAM_RATE, SND_PCM_HW_RATE_MIN, SND_PCM_HW_RATE_MAX },
        { SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PCM_PERIOD_MIN_FRAMES, period_max },
        { SNDRV_PCM_HW_PARAM_PERIODS, 1, PCM_MAX_PERIODS },
        { SNDRV_PCM_HW_PARAM_BUFFER_SIZE, PCM_PERIOD_MIN_FRAMES, PCM_MAX_PERIODS * period_max },
    };
    for (uint32_t i = 0; i < 7; i++) {
        changed = false;
        if (!refine_param_interval(&params->intervals[ivs[i][0] - 8], ivs[i][1], ivs[i][2], &changed)) return -EINVAL;
        if (changed) params->cmask |= 1u << ivs[i][0];
    }
    struct snd_interval *rate_iv = &params->intervals[SNDRV_PCM_HW_PARAM_RATE - 8];
    uint32_t rate_lo = rate_iv->min ? rate_iv->min : SND_PCM_HW_RATE_MIN;
    uint32_t rate_hi = rate_iv->max ? rate_iv->max : SND_PCM_HW_RATE_MAX;
    struct snd_interval *psize_iv = &params->intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - 8];
    struct snd_interval *bsize_iv = &params->intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - 8];
    // Reverse rules like the kernel's hw dependency rules: a narrowed
    // time narrows its size. alsa-lib negotiates through refine
    // round-trips and relies on this (set_buffer_time_near constrains the
    // buffer this way); without it the size stays wide until something
    // pins it to max, and the install struct contradicts itself.
    struct snd_interval *pttime_iv = &params->intervals[SNDRV_PCM_HW_PARAM_PERIOD_TIME - 8];
    struct snd_interval *bftime_iv = &params->intervals[SNDRV_PCM_HW_PARAM_BUFFER_TIME - 8];
    // A few frames of slack both ways: alsa-lib may hold TIME single
    // 42666 (for 2048 frames) while SIZE single is 2047 from an earlier
    // _near step. Strict ceil would fail refine with -EINVAL and plain
    // `aplay` would abort; install overwrites TIME anyway, so tolerate
    // 1-frame mismatches here. Huge mismatches (125ms vs 42ms max) still
    // fail and let _near backtrack to the maximum.
    #define REVERSE_SLACK_FRAMES 4
    if (!pttime_iv->empty && !psize_iv->empty) {
        uint32_t plo = (uint32_t)(((uint64_t)pttime_iv->min * rate_lo + 999999) / 1000000);
        uint32_t phi = (uint32_t)(((uint64_t)pttime_iv->max * rate_hi + 999999) / 1000000);
        plo = plo > REVERSE_SLACK_FRAMES ? plo - REVERSE_SLACK_FRAMES : 0;
        phi += REVERSE_SLACK_FRAMES;
        changed = false;
        if (!refine_param_interval(psize_iv, plo, phi, &changed)) return -EINVAL;
        if (changed) params->cmask |= 1u << SNDRV_PCM_HW_PARAM_PERIOD_SIZE;
    }
    if (!bftime_iv->empty && !bsize_iv->empty) {
        uint32_t blo = (uint32_t)(((uint64_t)bftime_iv->min * rate_lo + 999999) / 1000000);
        uint32_t bhi = (uint32_t)(((uint64_t)bftime_iv->max * rate_hi + 999999) / 1000000);
        blo = blo > REVERSE_SLACK_FRAMES ? blo - REVERSE_SLACK_FRAMES : 0;
        bhi += REVERSE_SLACK_FRAMES;
        changed = false;
        if (!refine_param_interval(bsize_iv, blo, bhi, &changed)) return -EINVAL;
        if (changed) params->cmask |= 1u << SNDRV_PCM_HW_PARAM_BUFFER_SIZE;
    }
    #undef REVERSE_SLACK_FRAMES
    uint32_t psize_lo = psize_iv->min;
    uint32_t psize_hi = psize_iv->max;
    uint32_t bsize_lo = bsize_iv->min;
    uint32_t bsize_hi = bsize_iv->max;
    // Forward TIME needs ~2 frames of slack so a 1-frame rounding
    // mismatch (e.g. size 2047 vs time 42666 for 2048) does not make
    // refine fail: install overwrites TIME from SIZE/RATE anyway.
    // Without slack, `aplay -D hw:0` fails at refine with -EINVAL.
    uint32_t slack = rate_lo ? (uint32_t)(2000000ULL / rate_lo + 1) : 100;
    #define CLAMP32(x) ((uint32_t)((x) > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (x)))
    uint32_t ptime_lo = (uint32_t)((uint64_t)psize_lo * 1000000 / rate_hi);
    uint32_t ptime_hi = (uint32_t)(((uint64_t)psize_hi * 1000000 + rate_lo - 1) / (rate_lo ? rate_lo : 1));
    uint32_t btime_lo = (uint32_t)((uint64_t)bsize_lo * 1000000 / rate_hi);
    uint32_t btime_hi = (uint32_t)(((uint64_t)bsize_hi * 1000000 + rate_lo - 1) / (rate_lo ? rate_lo : 1));
    ptime_lo = ptime_lo > slack ? ptime_lo - slack : 0;
    btime_lo = btime_lo > slack ? btime_lo - slack : 0;
    ptime_hi = CLAMP32((uint64_t)ptime_hi + slack);
    btime_hi = CLAMP32((uint64_t)btime_hi + slack);
    uint32_t sizes[][3] = {
        // Frame bytes vary 1 (U8 mono) .. 4 (S16 stereo); *4 for lo
        // over-constrains 8-bit/mono and fails `aplay -D hw:0`.
        { SNDRV_PCM_HW_PARAM_PERIOD_BYTES, CLAMP32((uint64_t)psize_lo * 1), CLAMP32((uint64_t)psize_hi * 4) },
        { SNDRV_PCM_HW_PARAM_BUFFER_BYTES, CLAMP32((uint64_t)bsize_lo * 1), CLAMP32((uint64_t)bsize_hi * 4) },
        { SNDRV_PCM_HW_PARAM_PERIOD_TIME, ptime_lo, ptime_hi },
        { SNDRV_PCM_HW_PARAM_BUFFER_TIME, btime_lo, btime_hi },
        { SNDRV_PCM_HW_PARAM_TICK_TIME, 0, 0 },
    };
    #undef CLAMP32
    for (uint32_t i = 0; i < 5; i++) {
        changed = false;
        if (!refine_param_interval(&params->intervals[sizes[i][0] - 8], sizes[i][1], sizes[i][2], &changed)) return -EINVAL;
        if (changed) params->cmask |= 1u << sizes[i][0];
    }
    // ALSA triple linkage: buffer == period * periods must hold for some
    // triple in range. Without this, alsa-lib/plug picks incompatible
    // singles (buffer 24000 + period 2046 => 11.73 periods) and ends with
    // open-empty PERIODS (11 12) that fails install. With this, HW_REFINE
    // fails early for incompatible and _near backtracks to a compatible
    // triple (e.g. 2000x12 for 24000) so plain `aplay` just works.
    for (int iter = 0; iter < 3; iter++) {
        bool any = false;
        struct snd_interval *piv2 = &params->intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - 8];
        struct snd_interval *niv2 = &params->intervals[SNDRV_PCM_HW_PARAM_PERIODS - 8];
        struct snd_interval *biv2 = &params->intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - 8];
        if (piv2->empty || niv2->empty || biv2->empty) return -EINVAL;
        uint32_t pmin2 = choose_interval_min(piv2), pmax2 = choose_interval_max(piv2);
        uint32_t nmin2 = choose_interval_min(niv2), nmax2 = choose_interval_max(niv2);
        uint32_t bmin2 = choose_interval_min(biv2), bmax2 = choose_interval_max(biv2);
        if (pmin2 < 1) pmin2 = 1;
        if (nmin2 < 1) nmin2 = 1;
        if (pmax2 == 0 || nmax2 == 0 || bmax2 == 0) return -EINVAL;
        // periods from buffer/period: n in [ceil(bmin/pmax), floor(bmax/pmin)]
        {
            uint32_t n_lo = (uint32_t)((bmin2 + pmax2 - 1) / pmax2);
            uint32_t n_hi = (uint32_t)(bmax2 / pmin2);
            if (n_lo < 1) n_lo = 1;
            changed = false;
            if (!refine_param_interval(niv2, n_lo, n_hi, &changed)) return -EINVAL;
            if (changed) { params->cmask |= 1u << SNDRV_PCM_HW_PARAM_PERIODS; any = true; }
        }
        // buffer from period*periods
        {
            uint64_t b_lo = (uint64_t)pmin2 * nmin2;
            uint64_t b_hi = (uint64_t)pmax2 * nmax2;
            if (b_lo < 1) b_lo = 1;
            if (b_hi > 0xFFFFFFFFULL) b_hi = 0xFFFFFFFFULL;
            // Re-read mins (periods may have narrowed above).
            pmin2 = choose_interval_min(piv2); pmax2 = choose_interval_max(piv2);
            nmin2 = choose_interval_min(niv2); nmax2 = choose_interval_max(niv2);
            b_lo = (uint64_t)pmin2 * nmin2;
            b_hi = (uint64_t)pmax2 * nmax2;
            if (b_lo < 1) b_lo = 1;
            if (b_hi > 0xFFFFFFFFULL) b_hi = 0xFFFFFFFFULL;
            changed = false;
            if (!refine_param_interval(biv2, (uint32_t)b_lo, (uint32_t)b_hi, &changed)) return -EINVAL;
            if (changed) { params->cmask |= 1u << SNDRV_PCM_HW_PARAM_BUFFER_SIZE; any = true; }
        }
        // period from buffer/periods: p in [ceil(bmin/nmax), floor(bmax/nmin)]
        {
            pmin2 = choose_interval_min(piv2); pmax2 = choose_interval_max(piv2);
            nmin2 = choose_interval_min(niv2); nmax2 = choose_interval_max(niv2);
            bmin2 = choose_interval_min(biv2); bmax2 = choose_interval_max(biv2);
            if (nmax2 == 0 || nmin2 == 0) return -EINVAL;
            uint32_t p_lo = (uint32_t)((bmin2 + nmax2 - 1) / nmax2);
            uint32_t p_hi = (uint32_t)(bmax2 / nmin2);
            if (p_lo < 1) p_lo = 1;
            changed = false;
            if (!refine_param_interval(piv2, p_lo, p_hi, &changed)) return -EINVAL;
            if (changed) { params->cmask |= 1u << SNDRV_PCM_HW_PARAM_PERIOD_SIZE; any = true; }
        }
        if (!any) break;
    }
    params->info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_SYNC_APPLPTR | SNDRV_PCM_INFO_PERFECT_DRAIN | SNDRV_PCM_INFO_INTERLEAVED;
    if (params->masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0] == (1u << SNDRV_PCM_FORMAT_U8)) params->msbits = 8;
    if (params->masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0] == (1u << SNDRV_PCM_FORMAT_S16_LE)) params->msbits = 16;
    struct snd_interval *riv = &params->intervals[SNDRV_PCM_HW_PARAM_RATE - 8];
    if (riv->min == riv->max && !riv->openmin && !riv->openmax && riv->min > 0) {
        params->rate_num = riv->min;
        params->rate_den = 1;
    }
    params->rmask = 0;
    return 0;
}
