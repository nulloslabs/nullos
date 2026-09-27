#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sound/asound.h>

#define SND_CTL_NUMID_VOLUME 1
#define SND_CTL_NUMID_SWITCH 2
#define SND_CTL_ELEM_COUNT   2

typedef struct {
    unsigned int numid;
    const char *name;
} snd_elem_def_t;

int register_snd_control(int card);
uint64_t read_snd_control(void *buf, uint64_t count, uint64_t offset, int index, void *handle);
uint64_t write_snd_control(const void *buf, uint64_t count, uint64_t offset, int index, void *handle);
int get_snd_card_info(int card, struct snd_ctl_card_info *info);
int get_snd_elem_id(int card, uint32_t idx, struct snd_ctl_elem_id *id);
int get_snd_elem_info(int card, struct snd_ctl_elem_info *info);
int read_snd_elem(int card, struct snd_ctl_elem_value *value);
int write_snd_elem(int card, const struct snd_ctl_elem_value *value);
