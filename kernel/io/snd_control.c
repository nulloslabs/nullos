#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <sound/asound.h>
#include <main/string.h>
#include <io/audio.h>
#include <io/devices.h>
#include <io/snd.h>
#include <io/snd_control.h>

static const snd_elem_def_t elem_defs[SND_CTL_ELEM_COUNT] = {
    { SND_CTL_NUMID_VOLUME, "Master Playback Volume" },
    { SND_CTL_NUMID_SWITCH, "Master Playback Switch" },
};

static const snd_card_t *check_snd_card(int card) {
    const snd_card_t *c = get_snd_card(card);
    if (!c || !c->present || c->driver == AUDIO_NONE) return 0;
    return c;
}

static void fill_snd_elem_id(struct snd_ctl_elem_id *id, int card, const snd_elem_def_t *def) {
    memset(id, 0, sizeof(*id));
    id->numid = def->numid;
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    id->device = 0;
    id->subdevice = 0;
    strncpy((char *)id->name, def->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN - 1);
    id->index = 0;
    (void)card;
}

static const snd_elem_def_t *find_snd_elem(const struct snd_ctl_elem_id *id) {
    if (!id) return 0;
    for (uint32_t i = 0; i < SND_CTL_ELEM_COUNT; i++) {
        if (id->numid != 0) {
            if (elem_defs[i].numid == id->numid) return &elem_defs[i];
            continue;
        }
        if (id->iface != SNDRV_CTL_ELEM_IFACE_MIXER || id->device != 0 || id->index != 0) continue;
        if (strncmp((const char *)id->name, elem_defs[i].name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN) != 0) continue;
        return &elem_defs[i];
    }
    return 0;
}

int register_snd_control(int card) {
    if (card < 0 || card > 9) return -EINVAL;
    char name[] = "snd/controlC0";
    name[12] = (char)('0' + card);
    return register_device(name, read_snd_control, write_snd_control);
}

uint64_t read_snd_control(void *buf, uint64_t count, uint64_t offset, int index, void *handle) {
    (void)handle;
    (void)buf;
    (void)count;
    (void)offset;
    (void)index;
    return 0;
}

uint64_t write_snd_control(const void *buf, uint64_t count, uint64_t offset, int index, void *handle) {
    (void)handle;
    (void)buf;
    (void)count;
    (void)offset;
    (void)index;
    return (uint64_t)-EPERM;
}

int get_snd_card_info(int card, struct snd_ctl_card_info *info) {
    const snd_card_t *c = check_snd_card(card);
    if (!c || !info) return -ENXIO;
    memset(info, 0, sizeof(*info));
    info->card = card;
    strncpy((char *)info->id, c->name, sizeof(info->id) - 1);
    strncpy((char *)info->driver, c->mixername, sizeof(info->driver) - 1);
    strncpy((char *)info->name, c->mixername, sizeof(info->name) - 1);
    strncpy((char *)info->longname, c->longname, sizeof(info->longname) - 1);
    strncpy((char *)info->mixername, c->mixername, sizeof(info->mixername) - 1);
    strncpy((char *)info->components, c->components, sizeof(info->components) - 1);
    return 0;
}

int get_snd_elem_id(int card, uint32_t idx, struct snd_ctl_elem_id *id) {
    if (!check_snd_card(card) || !id) return -ENXIO;
    if (idx >= SND_CTL_ELEM_COUNT) return -EINVAL;
    fill_snd_elem_id(id, card, &elem_defs[idx]);
    return 0;
}

int get_snd_elem_info(int card, struct snd_ctl_elem_info *info) {
    if (!check_snd_card(card) || !info) return -ENXIO;
    const snd_elem_def_t *def = find_snd_elem(&info->id);
    if (!def) return -ENOENT;
    memset(info, 0, sizeof(*info));
    fill_snd_elem_id(&info->id, card, def);
    if (def->numid == SND_CTL_NUMID_VOLUME) {
        info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
        info->access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
        info->count = 2;
        info->value.integer.min = 0;
        info->value.integer.max = 63;
        info->value.integer.step = 1;
    } else {
        info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
        info->access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
        info->count = 1;
        info->value.integer.min = 0;
        info->value.integer.max = 1;
        info->value.integer.step = 0;
    }
    return 0;
}

int read_snd_elem(int card, struct snd_ctl_elem_value *value) {
    if (!check_snd_card(card) || !value) return -ENXIO;
    const snd_elem_def_t *def = find_snd_elem(&value->id);
    if (!def) return -ENOENT;
    if (def->numid == SND_CTL_NUMID_VOLUME) {
        uint8_t left = 0;
        uint8_t right = 0;
        get_audio_volume(&left, &right);
        value->value.integer.value[0] = left;
        value->value.integer.value[1] = right;
    } else {
        value->value.integer.value[0] = is_audio_muted() ? 0 : 1;
    }
    return 0;
}

int write_snd_elem(int card, const struct snd_ctl_elem_value *value) {
    if (!check_snd_card(card) || !value) return -ENXIO;
    const snd_elem_def_t *def = find_snd_elem(&value->id);
    if (!def) return -ENOENT;
    if (def->numid == SND_CTL_NUMID_VOLUME) {
        long left = value->value.integer.value[0];
        long right = value->value.integer.value[1];
        if (left < 0) left = 0;
        if (left > 63) left = 63;
        if (right < 0) right = 0;
        if (right > 63) right = 63;
        return set_audio_volume((uint8_t)left, (uint8_t)right);
    }
    return mute_audio(value->value.integer.value[0] == 0);
}
