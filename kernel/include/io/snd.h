#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <main/fd.h>
#include <io/audio.h>

#define SND_MAX_CARDS          4
#define SND_WRITEI_CHUNK_BYTES 32768

typedef struct {
    char name[16];
    char longname[80];
    char mixername[80];
    char components[128];
    char pcm_name[80];
    audio_driver_t driver;
    bool present;
} snd_card_t;

void init_snd(void);
int register_snd_card(audio_driver_t driver);
const snd_card_t *get_snd_card(int card);
int handle_snd_ioctl(fd_entry_t *entry, const char *rel, unsigned long req, unsigned long arg);
