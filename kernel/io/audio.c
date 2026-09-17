#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <main/string.h>
#include <io/audio.h>
#include <io/ac97.h>

audio_driver_t current_audio_driver = AUDIO_NONE;

bool is_audio_playing(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return false; // This is a bool, just return false as it's not playing audio.
        case AUDIO_AC97:
            return is_ac97_playing();
        default:
            return false; // Achievement unlocked: How Did We Get Here?
    }
    return false;
}

bool is_audio_started(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return false;
        case AUDIO_AC97:
            return is_ac97_started();
        default:
            return false;
    }
    return false;
}

bool is_audio_muted(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return true; // Return true here since there's no sound card to play audio.
        case AUDIO_AC97:
            return is_ac97_muted();
        default:
            return true;
    }
    return true;
}

int set_audio_volume(uint8_t left, uint8_t right) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return -ENODEV;
        case AUDIO_AC97:
            return set_ac97_volume(left, right);
        default:
            return -EINVAL; // wtf
    }
    return -EINVAL;
}

void get_audio_volume(uint8_t *left, uint8_t *right) {
    if (left) *left = 0;
    if (right) *right = 0;
    if (current_audio_driver != AUDIO_AC97) return;
    get_ac97_volume(left, right);
}

int mute_audio(bool muted) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return -ENODEV;
        case AUDIO_AC97:
            return mute_ac97(muted);
        default:
            return -EINVAL;
    }
    return -EINVAL;
}

int set_audio_params(const audio_params_t *params) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return -ENODEV;
        case AUDIO_AC97:
            return set_ac97_params(params);
        default:
            return -EINVAL;
    }
    return -EINVAL;
}

uint32_t get_audio_rate(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_rate();
        default:
            return 0;
    }
    return 0;
}

int get_audio_driver_info(audio_driver_t driver, audio_driver_info_t *info) {
    if (!info) return -EINVAL;
    memset(info, 0, sizeof(*info));
    switch (driver) {
        case AUDIO_AC97:
            get_ac97_driver_info(info);
            return 0;
        default:
            return -ENODEV;
    }
    return -ENODEV;
}

int drain_audio(uint64_t appl_frames) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return flush_ac97(appl_frames);
        default:
            return -EINVAL;
    }
    return -EINVAL;
}

uint64_t get_audio_staged_frames(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_staged_frames();
        default:
            return 0;
    }
    return 0;
}

uint64_t get_audio_pending_frames(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_pending_frames();
        default:
            return 0;
    }
    return 0;
}

uint64_t get_audio_ring_bytes(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_ring_bytes();
        default:
            return 0;
    }
    return 0;
}

uint64_t get_audio_ring_phys(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_ring_phys();
        default:
            return 0;
    }
    return 0;
}

uint64_t get_audio_ring_frames(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_buffer_frames();
        default:
            return 0;
    }
    return 0;
}

uint64_t get_audio_period_frames(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_period_frames();
        default:
            return 0;
    }
    return 0;
}

uint32_t get_audio_max_period_frames(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return get_ac97_max_period_frames();
        default:
            return 0;
    }
    return 0;
}

int set_audio_start_threshold(uint64_t frames) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return set_ac97_start_threshold(frames);
        default:
            return -EINVAL;
    }
    return -EINVAL;
}

int commit_audio_frames(uint64_t appl_frames) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return commit_ac97_frames(appl_frames);
        default:
            return -EINVAL;
    }
    return -EINVAL;
}

int kick_audio(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return kick_ac97();
        default:
            return -EINVAL;
    }
    return -EINVAL;
}

void reset_audio_mmap(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return;
        case AUDIO_AC97:
            reset_ac97_mmap();
            break;
        default:
            return;
    }
}

void drop_audio(void) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return;
        case AUDIO_AC97:
            drop_ac97();
            break;
        default:
            return;
    }
}

int play_audio(void *buf, size_t size) {
    switch (current_audio_driver) {
        case AUDIO_NONE:
            return 0;
        case AUDIO_AC97:
            return play_ac97(buf, size);
        default:
            return -EINVAL;
    }
    return -EINVAL;
}
