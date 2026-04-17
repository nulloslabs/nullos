#include <freestanding/stdint.h>
#include <freestanding/stdarg.h>
#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>
#include <main/string.h>
#include <io/io.h>
#include <io/serial.h>

int g_debug_port = 0;

static void int_to_str(uint64_t value, char *buf, size_t buf_size, int base, bool uppercase) {
    char temp[64];
    int i = 0;

    // Ensure base is valid (default to 10 if invalid)
    if (base <= 0 || base > 36) base = 10;

    if (value == 0) {
        if (buf_size > 1) { buf[0] = '0'; buf[1] = '\0'; }
        return;
    }

    // Determine the letter offset: 'A' (65) for uppercase, 'a' (97) for lowercase
    char hex_offset = uppercase ? 'A' : 'a';

    while (value > 0 && i < 63) {
        uint64_t rem = value % base;
        // If rem is 10, (10 - 10 + 'A') = 'A'. Perfect.
        temp[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + hex_offset);
        value /= base;
    }

    int j = 0;
    while (i > 0 && j < (int)buf_size - 1) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

void serial_putc(uint16_t port, char c) {
    // Just in case if terminal dosen't support just "\n" for newlines but needs "\r\n" instead
    if (c == '\n') outb(port, '\r');
    outb(port, c);
}

void serial_puts(uint16_t port, const char *s) {
    while (*s) serial_putc(port, *s++);
}

void serial_printf(uint16_t port, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            serial_putc(port, *p);
            continue;
        }
        p++;
        if (*p == '\0') { serial_putc(port, '%'); break; }
        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        bool is_long = false;

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

        switch (*p) {
            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = strlen(s);
                if (!left_align)
                    while (width > len) { serial_putc(port, pad_char); width--; }
    
                while (*s) serial_putc(port, *s++);
    
                if (left_align)
                    while (width > len) { serial_putc(port, ' '); width--; }
                break;
            }
            case 'o': case 'O':
            case 'd': case 'D':
            case 'u': case 'U':
            case 'x': case 'X': {
                uint64_t val;
                bool forced_long = (*p == 'D' || *p == 'U' || *p == 'O');
                if (is_long || forced_long) {
                    val = va_arg(args, uint64_t);
                } else {
                    if (*p == 'd') val = (uint64_t)va_arg(args, int);
                    else           val = (uint64_t)va_arg(args, unsigned int);
                }
                bool is_neg = (*p == 'd' || *p == 'D') && (int64_t)val < 0;
                if (is_neg) val = -(int64_t)val;
                int base = (*p=='x'||*p=='X') ? 16 : (*p=='o'||*p=='O') ? 8 : 10;
                char buf[64];
                int_to_str(val, buf, 64, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++; // account for '-'
                if (!left_align)
                    while (width > len) { serial_putc(port, pad_char); width--; }
                if (is_neg) serial_putc(port, '-');
                char *ptr = buf;
                while (*ptr) serial_putc(port, *ptr++);
                if (left_align)
                    while (width > len) { serial_putc(port, ' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char buf[64];
                int_to_str(x, buf, 64, 16, false);
                serial_putc(port, '0'); serial_putc(port, 'x');
                int len = 0;
                while (buf[len]) len++;
                for (int i = 0; i < (16 - len); i++) serial_putc(port, '0');
                char *ptr = buf;
                while (*ptr) serial_putc(port, *ptr++);
                break;
            }
            case 'c':
                serial_putc(port, (char)va_arg(args, int));
                break;
            case '%':
                serial_putc(port, '%');
                break;
            default:
                serial_putc(port, '%');
                serial_putc(port, *p);
                break;
        }
    }
    va_end(args);
}
