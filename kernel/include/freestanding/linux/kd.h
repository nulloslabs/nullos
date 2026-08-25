#pragma once

#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define KDGKBENT  0x4B46
#define KDSKBENT  0x4B47
#define KDFONTOP  0x4B72

#define K_RAW       0x00
#define K_XLATE     0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE   0x03
#define K_OFF       0x04

#define KD_FONT_OP_SET           0
#define KD_FONT_FLAG_DONT_RECALC 1

struct console_font_op {
    unsigned int op;
    unsigned int flags;
    unsigned int width;
    unsigned int height;
    unsigned int charcount;
    unsigned char *data;
};

struct kbentry {
    unsigned char kb_table;
    unsigned char kb_index;
    unsigned short kb_value;
};
