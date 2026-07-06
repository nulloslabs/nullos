#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdarg.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/io.h>
#include <io/serial.h>
#include <io/terminal.h>

static spinlock_t serial_lock = SPINLOCK_INIT;
static const uint16_t serial_ports[] = { COM1, COM2, COM3, COM4 };

static void int_to_str(uint64_t value, char *buf, size_t buf_size, int base, bool uppercase) {
    char temp[64];
    int i = 0;

    // Ensure base is valid (default to 10 if invalid)
    if (base <= 0 || base > 36) base = 10;

    if (value == 0) { if (buf_size > 1) { buf[0] = '0'; buf[1] = '\0'; } return; }

    // Determine the letter offset: 'A' (65) for uppercase, 'a' (97) for lowercase
    char hex_offset = uppercase ? 'A' : 'a';

    while (value > 0 && i < 63) {
        uint64_t rem = value % base;
        // If rem is 10, (10 - 10 + 'A') = 'A'. Perfect.
        temp[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + hex_offset);
        value /= base;
    }

    int j = 0;
    while (i > 0 && j < (int)buf_size - 1) { buf[j++] = temp[--i]; }
    buf[j] = '\0';
}

static void double_to_str(double val, int precision, char *out, size_t out_size) {
    if (out_size < 2) return;

    uint64_t bits;
    __builtin_memcpy(&bits, &val, 8);

    int sign = (bits >> 63) & 1;
    int exp  = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0x000FFFFFFFFFFFFFULL;

    // special cases
    if (exp == 0x7FF) {
        if (mant == 0) {
            size_t i = 0;
            if (sign && i < out_size - 1) out[i++] = '-';
            const char *inf = "inf";
            while (*inf && i < out_size - 1) out[i++] = *inf++;
            out[i] = '\0';
        } else {
            size_t i = 0;
            const char *nan = "nan";
            while (*nan && i < out_size - 1) out[i++] = *nan++;
            out[i] = '\0';
        }
        return;
    }

    size_t pos = 0;
    if (sign && pos < out_size - 1) out[pos++] = '-';

    // reconstruct integer and fractional parts using integer arithmetic.
    // we work with the value as: significand * 2^(exp-1023-52)
    int e = exp - 1023 - 52;
    uint64_t sig = (exp == 0) ? mant : (mant | (1ULL << 52));

    uint64_t int_part;
    uint64_t frac_num, frac_den;

    if (e >= 0) {
        // value is an integer (or too large for frac precision)
        if (e < 63) int_part = sig << e; else int_part = 0xFFFFFFFFFFFFFFFFULL;
        frac_num = 0; frac_den = 1;
    } else if (e > -53) {
        int shift = -e;
        int_part  = sig >> shift;
        frac_num  = sig & ((1ULL << shift) - 1);
        frac_den  = 1ULL << shift;
    } else {
        int_part = 0;
        // value < 1; represent as frac_num/frac_den with capped shift
        int shift = (e < -62) ? 62 : -e;
        frac_num = sig >> (-e - shift);
        frac_den = 1ULL << shift;
    }

    // write integer part
    char ibuf[24]; int ilen = 0;
    if (int_part == 0) {
        ibuf[ilen++] = '0';
    } else {
        uint64_t tmp = int_part;
        while (tmp > 0 && ilen < 23) { ibuf[ilen++] = '0' + (int)(tmp % 10); tmp /= 10; }
        // reverse
        for (int a = 0, b = ilen - 1; a < b; a++, b--) { char t = ibuf[a]; ibuf[a] = ibuf[b]; ibuf[b] = t; }
    }
    for (int i = 0; i < ilen && pos < out_size - 1; i++) out[pos++] = ibuf[i];

    if (precision <= 0) { out[pos] = '\0'; return; }
    if (pos < out_size - 1) out[pos++] = '.';

    // write fractional digits
    for (int d = 0; d < precision && pos < out_size - 1; d++) {
        frac_num *= 10;
        uint64_t digit = frac_num / frac_den;
        frac_num %= frac_den;
        out[pos++] = '0' + (int)digit;
    }
    out[pos] = '\0';
}

static int serial_putchar_unlocked(uint16_t port, int c) {
    unsigned char ch = (unsigned char)c;
    // Just in case if terminal dosen't support just "\n" for newlines but needs "\r\n" instead
    if (ch == '\n') outb(port, '\r');
    outb(port, ch);
    return ch;
}

int serial_putchar(uint16_t port, int c) {
    uint64_t rflags;
    spin_lock_irqsave(&serial_lock, &rflags);
    int ret = serial_putchar_unlocked(port, c);
    spin_unlock_irqrestore(&serial_lock, rflags);
    return ret;
}

int serial_puts(uint16_t port, const char *s) {
    uint64_t rflags;
    spin_lock_irqsave(&serial_lock, &rflags);
    while (*s) { serial_putchar_unlocked(port, *s); s++; }
    spin_unlock_irqrestore(&serial_lock, rflags);
    return 0;
}

int serial_vprintf(uint16_t port, const char *fmt, va_list args) {
    int total_written = 0;
    uint64_t rflags;
    spin_lock_irqsave(&serial_lock, &rflags);

    #define PUTC(c) do { serial_putchar_unlocked(port, c); total_written++; } while(0)

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            PUTC(*p);
            continue;
        }
        p++;
        if (*p == '\0') { PUTC('%'); break; }
        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        bool is_long = false;
        bool is_size = false;

        // Left-align flag
        if (*p == '-') {
            left_align = true;
            p++;
        }
        // Zero-pad flag (ignored if left-aligning)
        if (*p == '0') {
            if (!left_align) pad_char = '0';
            p++;
        }
        // Width
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        // Long modifier
        if (*p == 'l') {
            is_long = true;
            p++;
            if (*p == 'l') p++;
        }
        // Size modifier (%z for size_t / ssize_t)
        if (*p == 'z') {
            is_size = true;
            p++;
        }

        switch (*p) {
            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = strlen(s);
                if (!left_align)
                    while (width > len) { PUTC(pad_char); width--; }

                while (*s) { PUTC(*s); s++; }

                if (left_align)
                    while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'o':
            case 'i':
            case 'd':
            case 'u':
            case 'b':
            case 'x': case 'X': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else if (is_size) {
                    val = (uint64_t)va_arg(args, size_t);
                } else {
                    if (*p == 'd' || *p == 'i') val = (uint64_t)va_arg(args, int);
                    else           val = (uint64_t)va_arg(args, unsigned int);
                }
                bool is_neg = (*p == 'd' || *p == 'D' || *p == 'i') && (int64_t)val < 0;
                if (is_neg) val = -(int64_t)val;
                int base = (*p=='x'||*p=='X') ? 16 : (*p=='o'||*p=='O') ? 8 : (*p=='b') ? 2 : 10;
                char buf[64];
                int_to_str(val, buf, 64, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++; // account for '-'
                if (!left_align)
                    while (width > len) { PUTC(pad_char); width--; }
                if (is_neg) PUTC('-');
                char *ptr = buf;
                while (*ptr) PUTC(*ptr++);
                if (left_align)
                    while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char buf[64];
                int_to_str(x, buf, 64, 16, false);
                PUTC('0'); PUTC('x');
                int len = 0;
                while (buf[len]) len++;
                for (int i = 0; i < (16 - len); i++) PUTC('0');
                char *ptr = buf;
                while (*ptr) PUTC(*ptr++);
                break;
            }
            case 'f': case 'F': {
                double fval = va_arg(args, double);
                int prec = 6;
                char fbuf[64];
                double_to_str(fval, prec, fbuf, sizeof(fbuf));
                int len = 0; while (fbuf[len]) len++;
                if (!left_align)
                    while (width > len) { PUTC(pad_char); width--; }
                char *fp = fbuf; while (*fp) PUTC(*fp++);
                if (left_align)
                    while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'c':
                PUTC((char)va_arg(args, int));
                break;
            case '%':
                PUTC('%');
                break;
            default:
                PUTC('%');
                PUTC(*p);
                break;
        }
    }
    
    #undef PUTC

    spin_unlock_irqrestore(&serial_lock, rflags);
    return total_written;
}

int serial_printf(uint16_t port, const char *fmt, ...) {
    // No spinlocks here since vprintf already has spinlocks
    va_list args;
    va_start(args, fmt);
    int ret = serial_vprintf(port, fmt, args);
    va_end(args);
    return ret;
}

void init_serial_ports(void) {
    // The reason why we do this is because most PCs expect us to initialize the serial ports.
    // Funny enough, QEMU dosen't need this (for some reason).
    for (int i = 0; i < (int)(sizeof(serial_ports) / sizeof(serial_ports[0])); i++) {
        uint16_t port = serial_ports[i]; // This is our port value.

        // Initialize the port.
        outb(port + 1, 0x00);
        outb(port + 3, 0x80);
        outb(port + 0, 0x03);
        outb(port + 1, 0x00);
        outb(port + 3, 0x03);
        outb(port + 2, 0xC7);
        outb(port + 4, 0x0B);
        outb(port + 4, 0x1E);
        outb(port + 0, 0xAE);

        if (inb(port + 0) != 0xAE) continue; // Check if port is faulty/not present.

        // Enable the port for use.
        outb(port + 4, 0x0F);
    }
    printf("serial: initialized serial ports\n");
}
