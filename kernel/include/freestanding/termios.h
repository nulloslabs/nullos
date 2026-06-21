#pragma once

#include <freestanding/stdint.h>

#define NCCS 32

// c_iflag bits
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

// c_oflag bits
#define OPOST   0000001
#define ONLCR   0000004
#define OLCUC   0000002

// c_cflag bits
#define CBAUD   0010017
#define CSIZE   0000060
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000

// c_lflag bits
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0010000

// c_cc indices
#define VINTR   0   // Ctrl+C  (SIGINT)
#define VQUIT   1   // Ctrl+\  (SIGQUIT)
#define VERASE  2   // Backspace
#define VKILL   3   // Ctrl+U  (kill line)
#define VEOF    4   // Ctrl+D  (EOF)
#define VTIME   5
#define VMIN    6
#define VSWTC   7
#define VSTART  8   // Ctrl+Q  (resume)
#define VSTOP   9   // Ctrl+S  (stop)
#define VSUSP  10   // Ctrl+Z  (SIGTSTP)
#define VEOL   11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

// tcsetattr optional_actions
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

// tcflush queue_selector
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

// tcflow action
#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

typedef uint32_t tcflag_t;
typedef uint8_t cc_t;
typedef uint32_t speed_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};
