#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <main/log.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/io.h>
#include <io/serial.h>

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

static int serial_putchar_unlocked(uint16_t port, int c) {
    unsigned char ch = (unsigned char)c;
    // Just in case if terminal dosen't support just "\n" for newlines but needs "\r\n" instead
    if (ch == '\n') serial_putchar_unlocked(port, '\r');
    while (!(inb(port + 5) & 0x20));
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
        int length_modifier = 0;

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
        if (*p == 'l') {
            length_modifier = 1;
            p++;
            if (*p == 'l') {
                length_modifier = 2;
                p++;
            }
        } else if (*p == 'z') {
            length_modifier = 3;
            p++;
        }

        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
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
            case 'x': case 'X': {
                bool is_signed = *p == 'd' || *p == 'i';
                bool is_neg = false;
                uint64_t val = 0;
                if (is_signed) {
                    int64_t signed_val;
                    if (length_modifier == 2) signed_val = va_arg(args, long long);
                    else if (length_modifier == 1) signed_val = va_arg(args, long);
                    else if (length_modifier == 3) signed_val = va_arg(args, ptrdiff_t);
                    else signed_val = va_arg(args, int);
                    is_neg = signed_val < 0;
                    val = is_neg ? (uint64_t)(-(signed_val + 1)) + 1 : (uint64_t)signed_val;
                } else {
                    if (length_modifier == 2) val = va_arg(args, unsigned long long);
                    else if (length_modifier == 1) val = va_arg(args, unsigned long);
                    else if (length_modifier == 3) val = va_arg(args, size_t);
                    else val = va_arg(args, unsigned int);
                }
                int base = (*p == 'x' || *p == 'X') ? 16 : *p == 'o' ? 8 : 10;
                char buf[64];
                int_to_str(val, buf, 64, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++; // account for '-'
                if (!left_align && pad_char == '0' && is_neg) { PUTC('-'); is_neg = false; }
                if (!left_align) while (width > len) { PUTC(pad_char); width--; }
                if (is_neg) PUTC('-');
                char *ptr = buf;
                while (*ptr) PUTC(*ptr++);
                if (left_align) while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = (uint64_t)(uintptr_t)va_arg(args, void *);
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
    log("serial: initialized serial ports\n");
}
