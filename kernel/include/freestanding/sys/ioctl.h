#pragma once

#define KDFONTOP 0x4B72
#define KDGKBENT 0x4B46
#define KDSKBENT 0x4B47
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TCSBRK 0x5409
#define TCXONC 0x540A
#define TCFLSH 0x540B
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define FIONREAD 0x541B
#define TIOCEXCL 0x540C
#define TIOCNXCL 0x540D
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCNOTTY 0x5422
#define TCGETS2 0x802C542A
#define TCSETS2 0x402C542B
#define TCSETSW2 0x402C542C
#define TCSETSF2 0x402C542D
#define TIOCSPTLCK 0x40045431
#define TIOCGPTN 0x80045430

#define KD_FONT_OP_SET 0
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
