#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>

int tcgetattr(int fd, struct termios *termios_p) {
    return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    int cmd;
    switch (optional_actions) {
        case TCSANOW:   cmd = TCSETS;  break;
        case TCSADRAIN: cmd = TCSETSW; break;
        case TCSAFLUSH: cmd = TCSETSF; break;
        default:
            errno = EINVAL;
            return -1;
    }
    return ioctl(fd, cmd, termios_p);
}

int tcsendbreak(int fd, int duration) {
    return ioctl(fd, 0x5409, duration); // TCSBRK
}

int tcdrain(int fd) {
    return ioctl(fd, 0x5409, 1); // TCSBRK with arg 1
}

int tcflush(int fd, int queue_selector) {
    return ioctl(fd, 0x540B, queue_selector); // TCFLSH
}

int tcflow(int fd, int action) {
    return ioctl(fd, 0x540A, action); // TCXONC
}

void cfmakeraw(struct termios *termios_p) {
    termios_p->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag &= ~(CSIZE | PARENB);
    termios_p->c_cflag |= CS8;
}

speed_t cfgetispeed(const struct termios *termios_p) {
    return termios_p->c_ispeed;
}

speed_t cfgetospeed(const struct termios *termios_p) {
    return termios_p->c_ospeed;
}

int cfsetispeed(struct termios *termios_p, speed_t speed) {
    termios_p->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *termios_p, speed_t speed) {
    termios_p->c_ospeed = speed;
    return 0;
}

int cfsetspeed(struct termios *termios_p, speed_t speed) {
    termios_p->c_ispeed = speed;
    termios_p->c_ospeed = speed;
    return 0;
}
