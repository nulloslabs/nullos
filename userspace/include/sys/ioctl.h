#pragma once

#define TIOCGWINSZ 0x0001
#define TIOCSWINSZ 0x0002
#define TCGETS 0x0003
#define TCSETS 0x0004
#define TCSETSW 0x0005
#define TCSETSF 0x0006
#define TIOCGPGRP 0x0007
#define TIOCSPGRP 0x0008
#define FIONREAD 0x0009
#define TIOCEXCL 0x0010
#define TIOCNXCL 0x0011

int ioctl(int fd, unsigned long op, ...);
