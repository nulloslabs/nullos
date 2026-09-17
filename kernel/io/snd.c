#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <io/audio.h>
#include <io/snd.h>
#include <io/snd_control.h>
#include <io/snd_pcm.h>
#include <io/snd_timer.h>

static snd_card_t cards[SND_MAX_CARDS];

const snd_card_t *get_snd_card(int card) {
    if (card < 0 || card >= SND_MAX_CARDS) return 0;
    if (!cards[card].present) return 0;
    return &cards[card];
}

int register_snd_card(audio_driver_t driver) {
    if (driver == AUDIO_NONE) return -ENODEV;
    audio_driver_info_t info;
    if (get_audio_driver_info(driver, &info) < 0) return -ENODEV;
    for (int i = 0; i < SND_MAX_CARDS; i++) {
        if (cards[i].present) continue;
        memset(&cards[i], 0, sizeof(cards[i]));
        strncpy(cards[i].name, info.name, sizeof(cards[i].name) - 1);
        strncpy(cards[i].longname, info.longname, sizeof(cards[i].longname) - 1);
        strncpy(cards[i].mixername, info.mixername, sizeof(cards[i].mixername) - 1);
        strncpy(cards[i].components, info.components, sizeof(cards[i].components) - 1);
        strncpy(cards[i].pcm_name, info.pcm_name, sizeof(cards[i].pcm_name) - 1);
        cards[i].driver = driver;
        cards[i].present = true;
        int rc = register_snd_control(i);
        if (rc < 0) { cards[i].present = false; return rc; }
        rc = register_snd_pcm(i, 0);
        if (rc < 0) { cards[i].present = false; return rc; }
        return i;
    }
    return -ENOMEM;
}

void init_snd(void) {
    register_snd_timer();
    if (current_audio_driver == AUDIO_NONE) return;
    register_snd_card(current_audio_driver);
    log("snd: initialized snd\n");
}
