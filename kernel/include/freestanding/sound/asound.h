// NullOS ALSA-compatible sound ABI (subset of Linux sound/asound.h).
// Struct layouts and ioctl numbers match Linux x86_64 exactly.

#pragma once

#include <stdint.h>
#include <time.h>

#define SNDRV_LITTLE_ENDIAN
#define SNDRV_MASK_MAX                256
#define AES_IEC958_STATUS_SIZE        24
#define SNDRV_CTL_ELEM_ID_NAME_MAXLEN 44

#define SNDRV_PCM_ACCESS_MMAP_INTERLEAVED    0
#define SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED 1
#define SNDRV_PCM_ACCESS_MMAP_COMPLEX        2
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED      3
#define SNDRV_PCM_ACCESS_RW_NONINTERLEAVED   4
#define SNDRV_PCM_ACCESS_LAST                SNDRV_PCM_ACCESS_RW_NONINTERLEAVED

#define SNDRV_PCM_FORMAT_S8      0
#define SNDRV_PCM_FORMAT_U8      1
#define SNDRV_PCM_FORMAT_S16_LE  2
#define SNDRV_PCM_FORMAT_S16_BE  3
#define SNDRV_PCM_FORMAT_U16_LE  4
#define SNDRV_PCM_FORMAT_U16_BE  5
#define SNDRV_PCM_FORMAT_S24_LE  6
#define SNDRV_PCM_FORMAT_S24_BE  7
#define SNDRV_PCM_FORMAT_U24_LE  8
#define SNDRV_PCM_FORMAT_U24_BE  9
#define SNDRV_PCM_FORMAT_S32_LE  10
#define SNDRV_PCM_FORMAT_S32_BE  11
#define SNDRV_PCM_FORMAT_U32_LE  12
#define SNDRV_PCM_FORMAT_U32_BE  13
#define SNDRV_PCM_FORMAT_SPECIAL 31
#define SNDRV_PCM_FORMAT_LAST    SNDRV_PCM_FORMAT_U32_BE
#define SNDRV_PCM_FORMAT_FIRST   SNDRV_PCM_FORMAT_S8

#ifdef SNDRV_LITTLE_ENDIAN
#define SNDRV_PCM_FORMAT_S16 SNDRV_PCM_FORMAT_S16_LE
#define SNDRV_PCM_FORMAT_U16 SNDRV_PCM_FORMAT_U16_LE
#define SNDRV_PCM_FORMAT_S24 SNDRV_PCM_FORMAT_S24_LE
#define SNDRV_PCM_FORMAT_U24 SNDRV_PCM_FORMAT_U24_LE
#define SNDRV_PCM_FORMAT_S32 SNDRV_PCM_FORMAT_S32_LE
#define SNDRV_PCM_FORMAT_U32 SNDRV_PCM_FORMAT_U32_LE
#endif

#define SNDRV_PCM_SUBFORMAT_STD  0
#define SNDRV_PCM_SUBFORMAT_LAST SNDRV_PCM_SUBFORMAT_STD

#define SNDRV_PCM_TSTAMP_NONE          0
#define SNDRV_PCM_TSTAMP_ENABLE        1
#define SNDRV_PCM_TSTAMP_MONOTONIC     2
#define SNDRV_PCM_TSTAMP_MONOTONIC_RAW 3
#define SNDRV_PCM_TSTAMP_LAST          SNDRV_PCM_TSTAMP_MONOTONIC_RAW

#define SNDRV_PCM_INFO_MMAP          0x00000001
#define SNDRV_PCM_INFO_MMAP_VALID    0x00000002
#define SNDRV_PCM_INFO_SYNC_APPLPTR  0x00000020
#define SNDRV_PCM_INFO_PERFECT_DRAIN 0x00000040
#define SNDRV_PCM_INFO_INTERLEAVED   0x00000100
#define SNDRV_PCM_STATE_OPEN         0
#define SNDRV_PCM_STATE_SETUP        1
#define SNDRV_PCM_STATE_PREPARED     2
#define SNDRV_PCM_STATE_RUNNING      3
#define SNDRV_PCM_STATE_XRUN         4
#define SNDRV_PCM_STATE_DRAINING     5
#define SNDRV_PCM_STATE_PAUSED       6
#define SNDRV_PCM_STATE_SUSPENDED    7
#define SNDRV_PCM_STATE_DISCONNECTED 8
#define SNDRV_PCM_STATE_LAST         SNDRV_PCM_STATE_DISCONNECTED

#define SNDRV_PCM_HW_PARAM_ACCESS         0
#define SNDRV_PCM_HW_PARAM_FORMAT         1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT      2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK     SNDRV_PCM_HW_PARAM_ACCESS
#define SNDRV_PCM_HW_PARAM_LAST_MASK      SNDRV_PCM_HW_PARAM_SUBFORMAT
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS    8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS     9
#define SNDRV_PCM_HW_PARAM_CHANNELS       10
#define SNDRV_PCM_HW_PARAM_RATE           11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME    12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE    13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES   14
#define SNDRV_PCM_HW_PARAM_PERIODS        15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME    16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE    17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES   18
#define SNDRV_PCM_HW_PARAM_TICK_TIME      19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL SNDRV_PCM_HW_PARAM_SAMPLE_BITS
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL  SNDRV_PCM_HW_PARAM_TICK_TIME

#define SNDRV_PCM_MMAP_OFFSET_DATA        0x00000000
#define SNDRV_PCM_MMAP_OFFSET_STATUS_OLD  0x80000000
#define SNDRV_PCM_MMAP_OFFSET_CONTROL_OLD 0x81000000
#define SNDRV_PCM_MMAP_OFFSET_STATUS_NEW  0x82000000
#define SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW 0x83000000
#define SNDRV_PCM_MMAP_OFFSET_STATUS      SNDRV_PCM_MMAP_OFFSET_STATUS_NEW
#define SNDRV_PCM_MMAP_OFFSET_CONTROL     SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW

#define SNDRV_PCM_SYNC_PTR_HWSYNC    (1 << 0)
#define SNDRV_PCM_SYNC_PTR_APPL      (1 << 1)
#define SNDRV_PCM_SYNC_PTR_AVAIL_MIN (1 << 2)

#define SNDRV_PCM_IOCTL_PVERSION      0x80044100
#define SNDRV_PCM_IOCTL_INFO          0x81204101
#define SNDRV_PCM_IOCTL_TSTAMP        0x40044102
#define SNDRV_PCM_IOCTL_TTSTAMP       0x40044103
#define SNDRV_PCM_IOCTL_USER_PVERSION 0x40044104
#define SNDRV_PCM_IOCTL_HW_REFINE     0xC2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS     0xC2604111
#define SNDRV_PCM_IOCTL_HW_FREE       0x4112
#define SNDRV_PCM_IOCTL_SW_PARAMS     0xC0884113
#define SNDRV_PCM_IOCTL_STATUS        0x80984120
#define SNDRV_PCM_IOCTL_DELAY         0x80084121
#define SNDRV_PCM_IOCTL_HWSYNC        0x4122
#define SNDRV_PCM_IOCTL_SYNC_PTR      0xC0884123
#define SNDRV_PCM_IOCTL_PREPARE       0x4140
#define SNDRV_PCM_IOCTL_RESET         0x4141
#define SNDRV_PCM_IOCTL_START         0x4142
#define SNDRV_PCM_IOCTL_DROP          0x4143
#define SNDRV_PCM_IOCTL_DRAIN         0x4144
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES 0x40184150

#define SNDRV_CTL_ELEM_TYPE_NONE       0
#define SNDRV_CTL_ELEM_TYPE_BOOLEAN    1
#define SNDRV_CTL_ELEM_TYPE_INTEGER    2
#define SNDRV_CTL_ELEM_TYPE_ENUMERATED 3
#define SNDRV_CTL_ELEM_TYPE_BYTES      4
#define SNDRV_CTL_ELEM_TYPE_IEC958     5
#define SNDRV_CTL_ELEM_TYPE_INTEGER64  6
#define SNDRV_CTL_ELEM_TYPE_LAST       SNDRV_CTL_ELEM_TYPE_INTEGER64
#define SNDRV_CTL_ELEM_IFACE_CARD      0
#define SNDRV_CTL_ELEM_IFACE_HWDEP     1
#define SNDRV_CTL_ELEM_IFACE_MIXER     2
#define SNDRV_CTL_ELEM_IFACE_PCM       3
#define SNDRV_CTL_ELEM_IFACE_RAWMIDI   4
#define SNDRV_CTL_ELEM_IFACE_TIMER     5
#define SNDRV_CTL_ELEM_IFACE_SEQUENCER 6
#define SNDRV_CTL_ELEM_IFACE_LAST      SNDRV_CTL_ELEM_IFACE_SEQUENCER

#define SNDRV_CTL_ELEM_ACCESS_READ      (1 << 0)
#define SNDRV_CTL_ELEM_ACCESS_WRITE     (1 << 1)
#define SNDRV_CTL_ELEM_ACCESS_READWRITE (SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_WRITE)
#define SNDRV_CTL_ELEM_ACCESS_INACTIVE  (1 << 8)

#define SNDRV_CTL_IOCTL_PVERSION             0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO            0x81785501
#define SNDRV_CTL_IOCTL_ELEM_LIST            0xC0505510
#define SNDRV_CTL_IOCTL_ELEM_INFO            0xC1105511
#define SNDRV_CTL_IOCTL_ELEM_READ            0xC4C85512
#define SNDRV_CTL_IOCTL_ELEM_WRITE           0xC4C85513
#define SNDRV_CTL_IOCTL_PCM_INFO             0xC1205531
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE      0x80045530
#define SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE 0x40045532

#define SNDRV_PROTOCOL_VERSION(major, minor, subminor) (((major) << 16) | ((minor) << 8) | (subminor))
#define SNDRV_PCM_VERSION    SNDRV_PROTOCOL_VERSION(2, 0, 18)
#define SNDRV_CTL_VERSION    SNDRV_PROTOCOL_VERSION(2, 0, 10)
#define SNDRV_TIMER_VERSION  SNDRV_PROTOCOL_VERSION(2, 0, 8)

#define SNDRV_TIMER_GLOBAL_SYSTEM  0
#define SNDRV_TIMER_GLOBAL_RTC     1
#define SNDRV_TIMER_GLOBAL_HPET    2
#define SNDRV_TIMER_GLOBAL_HRTIMER 3
#define SNDRV_TIMER_GLOBAL_UDRIVEN 4

#define SNDRV_TIMER_FLG_SLAVE (1 << 0)

#define SNDRV_TIMER_PSFLG_AUTO        (1 << 0)
#define SNDRV_TIMER_PSFLG_EXCLUSIVE   (1 << 1)
#define SNDRV_TIMER_PSFLG_EARLY_EVENT (1 << 2)

#define SNDRV_TIMER_IOCTL_PVERSION      0x80045400
#define SNDRV_TIMER_IOCTL_NEXT_DEVICE   0xC0145401
#define SNDRV_TIMER_IOCTL_GINFO         0xC0F85403
#define SNDRV_TIMER_IOCTL_GPARAMS       0x40485404
#define SNDRV_TIMER_IOCTL_GSTATUS       0xC0505405
#define SNDRV_TIMER_IOCTL_SELECT        0x40345410
#define SNDRV_TIMER_IOCTL_INFO          0x80E85411
#define SNDRV_TIMER_IOCTL_PARAMS        0x40505412
#define SNDRV_TIMER_IOCTL_STATUS        0x80605414
#define SNDRV_TIMER_IOCTL_START         0x54A0
#define SNDRV_TIMER_IOCTL_STOP          0x54A1
#define SNDRV_TIMER_IOCTL_CONTINUE      0x54A2
#define SNDRV_TIMER_IOCTL_PAUSE         0x54A3
#define SNDRV_TIMER_IOCTL_TREAD       0x40045402

typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
typedef int snd_pcm_access_t;
typedef int snd_pcm_format_t;
typedef int snd_pcm_subformat_t;
typedef int snd_pcm_state_t;
typedef int snd_pcm_hw_param_t;
typedef int snd_ctl_elem_type_t;
typedef int snd_ctl_elem_iface_t;

enum {
    SNDRV_PCM_STREAM_PLAYBACK = 0,
    SNDRV_PCM_STREAM_CAPTURE,
    SNDRV_PCM_STREAM_LAST = SNDRV_PCM_STREAM_CAPTURE,
};

enum {
    SNDRV_TIMER_CLASS_NONE = -1,
    SNDRV_TIMER_CLASS_SLAVE = 0,
    SNDRV_TIMER_CLASS_GLOBAL,
    SNDRV_TIMER_CLASS_CARD,
    SNDRV_TIMER_CLASS_PCM,
    SNDRV_TIMER_CLASS_LAST = SNDRV_TIMER_CLASS_PCM,
};

enum {
    SNDRV_TIMER_SCLASS_NONE = 0,
    SNDRV_TIMER_SCLASS_APPLICATION,
    SNDRV_TIMER_SCLASS_SEQUENCER,
    SNDRV_TIMER_SCLASS_OSS_SEQUENCER,
    SNDRV_TIMER_SCLASS_LAST = SNDRV_TIMER_SCLASS_OSS_SEQUENCER,
};

enum {
    SNDRV_TIMER_EVENT_RESOLUTION = 0,
    SNDRV_TIMER_EVENT_TICK,
    SNDRV_TIMER_EVENT_START,
    SNDRV_TIMER_EVENT_STOP,
    SNDRV_TIMER_EVENT_CONTINUE,
    SNDRV_TIMER_EVENT_PAUSE,
    SNDRV_TIMER_EVENT_SUSPEND,
    SNDRV_TIMER_EVENT_RESUME,
};

struct snd_pcm_info {
    unsigned int device;
    unsigned int subdevice;
    int stream;
    int card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned char subname[32];
    int dev_class;
    int dev_subclass;
    unsigned int subdevices_count;
    unsigned int subdevices_avail;
    unsigned char pad1[16];
    unsigned char reserved[64];
};

struct snd_interval {
    unsigned int min;
    unsigned int max;
    unsigned int openmin:1;
    unsigned int openmax:1;
    unsigned int integer:1;
    unsigned int empty:1;
};

struct snd_mask {
    uint32_t bits[(SNDRV_MASK_MAX + 31) / 32];
};

struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1];
    struct snd_mask mres[5];
    struct snd_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
    struct snd_interval ires[9];
    unsigned int rmask;
    unsigned int cmask;
    unsigned int info;
    unsigned int msbits;
    unsigned int rate_num;
    unsigned int rate_den;
    snd_pcm_uframes_t fifo_size;
    unsigned char sync[16];
    unsigned char reserved[48];
};

struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    snd_pcm_uframes_t avail_min;
    snd_pcm_uframes_t xfer_align;
    snd_pcm_uframes_t start_threshold;
    snd_pcm_uframes_t stop_threshold;
    snd_pcm_uframes_t silence_threshold;
    snd_pcm_uframes_t silence_size;
    snd_pcm_uframes_t boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};

struct snd_pcm_status {
    snd_pcm_state_t state;
    unsigned int pad1;
    struct timespec trigger_tstamp;
    struct timespec tstamp;
    snd_pcm_uframes_t appl_ptr;
    snd_pcm_uframes_t hw_ptr;
    snd_pcm_sframes_t delay;
    snd_pcm_uframes_t avail;
    snd_pcm_uframes_t avail_max;
    snd_pcm_uframes_t overrange;
    snd_pcm_state_t suspended_state;
    uint32_t audio_tstamp_data;
    struct timespec audio_tstamp;
    struct timespec driver_tstamp;
    uint32_t audio_tstamp_accuracy;
    unsigned char reserved[20];
};

struct snd_xferi {
    snd_pcm_sframes_t result;
    void *buf;
    snd_pcm_uframes_t frames;
};

struct snd_pcm_mmap_status {
    snd_pcm_state_t state;
    int pad1;
    snd_pcm_uframes_t hw_ptr;
    struct timespec tstamp;
    snd_pcm_state_t suspended_state;
    struct timespec audio_tstamp;
};

struct snd_pcm_mmap_control {
    snd_pcm_uframes_t appl_ptr;
    snd_pcm_uframes_t avail_min;
};

struct snd_pcm_sync_ptr {
    unsigned int flags;
    union {
        struct snd_pcm_mmap_status status;
        unsigned char reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        unsigned char reserved[64];
    } c;
};

struct snd_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
};

struct snd_ctl_elem_id {
    unsigned int numid;
    snd_ctl_elem_iface_t iface;
    unsigned int device;
    unsigned int subdevice;
    unsigned char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
    unsigned int index;
};

struct snd_ctl_elem_list {
    unsigned int offset;
    unsigned int space;
    unsigned int used;
    unsigned int count;
    struct snd_ctl_elem_id *pids;
    unsigned char reserved[50];
};

struct snd_ctl_elem_info {
    struct snd_ctl_elem_id id;
    snd_ctl_elem_type_t type;
    unsigned int access;
    unsigned int count;
    int owner;
    union {
        struct {
            long min;
            long max;
            long step;
        } integer;
        struct {
            long long min;
            long long max;
            long long step;
        } integer64;
        struct {
            unsigned int items;
            unsigned int item;
            char name[64];
            uint64_t names_ptr;
            unsigned int names_length;
        } enumerated;
        unsigned char reserved[128];
    } value;
    unsigned char reserved[64];
};

struct snd_aes_iec958 {
    unsigned char status[AES_IEC958_STATUS_SIZE];
    unsigned char subcode[147];
    unsigned char pad;
    unsigned char dig_subframe[4];
};

struct snd_ctl_elem_value {
    struct snd_ctl_elem_id id;
    unsigned int indirect: 1;
    union {
        union {
            long value[128];
            long *value_ptr;
        } integer;
        union {
            long long value[64];
            long long *value_ptr;
        } integer64;
        union {
            unsigned int item[128];
            unsigned int *item_ptr;
        } enumerated;
        union {
            unsigned char data[512];
            unsigned char *data_ptr;
        } bytes;
        struct snd_aes_iec958 iec958;
    } value;
    unsigned char reserved[128];
};

struct snd_timer_id {
    int dev_class;
    int dev_sclass;
    int card;
    int device;
    int subdevice;
};

struct snd_timer_ginfo {
    struct snd_timer_id tid;
    unsigned int flags;
    int card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned long reserved0;
    unsigned long resolution;
    unsigned long resolution_min;
    unsigned long resolution_max;
    unsigned int clients;
    unsigned char reserved[32];
};

struct snd_timer_gparams {
    struct snd_timer_id tid;
    unsigned long period_num;
    unsigned long period_den;
    unsigned char reserved[32];
};

struct snd_timer_gstatus {
    struct snd_timer_id tid;
    unsigned long resolution;
    unsigned long resolution_num;
    unsigned long resolution_den;
    unsigned char reserved[32];
};

struct snd_timer_select {
    struct snd_timer_id id;
    unsigned char reserved[32];
};

struct snd_timer_info {
    unsigned int flags;
    int card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned long reserved0;
    unsigned long resolution;
    unsigned char reserved[64];
};

struct snd_timer_params {
    unsigned int flags;
    unsigned int ticks;
    unsigned int queue_size;
    unsigned int reserved0;
    unsigned int filter;
    unsigned char reserved[60];
};

struct snd_timer_status {
    struct timespec tstamp;
    unsigned int resolution;
    unsigned int lost;
    unsigned int overrun;
    unsigned int queue;
    unsigned char reserved[64];
};

struct snd_timer_read {
    unsigned int resolution;
    unsigned int ticks;
};

struct snd_timer_tread {
    int event;
    unsigned int pad1;
    struct timespec tstamp;
    unsigned int val;
    unsigned int pad2;
};
