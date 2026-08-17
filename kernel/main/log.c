#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <main/log.h>
#include <main/boot_args.h>
#include <main/spinlocks.h>
#include <main/string.h>
#include <io/terminal.h>

static char log_text[LOG_TEXT_SIZE];
static log_record_t log_records[LOG_RECORD_COUNT];

static size_t record_head = 0;
static size_t record_count = 0;
static size_t text_head = 0;
static size_t text_used = 0;
static uint64_t next_sequence = 0;
static uint64_t first_text_sequence = 0;
static uint64_t next_text_sequence = 0;
static uint64_t stream_read_sequence = 0;
static uint64_t clear_sequence = 0;
static uint64_t dropped_records = 0;
static int console_level = 7;
static int saved_console_level = 7;
static bool is_console_initialized = false;
static spinlock_t log_lock = SPINLOCK_INIT;

static void format_putc(format_output_t *out, char c) {
    if (out->stored + 1 < out->capacity) {
        out->buf[out->stored++] = c;
    }
    out->total++;
}

static void format_repeat(format_output_t *out, char c, int count) {
    while (count-- > 0) format_putc(out, c);
}

static size_t format_unsigned(char *buf, size_t capacity, uint64_t value, unsigned int base, bool uppercase) {
    char reversed[64];
    size_t length = 0;
    const char *digits = uppercase
        ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        : "0123456789abcdefghijklmnopqrstuvwxyz";

    if (base < 2 || base > 36) base = 10;
    do {
        reversed[length++] = digits[value % base];
        value /= base;
    } while (value && length < sizeof(reversed));

    size_t written = 0;
    while (length && written < capacity) {
        buf[written++] = reversed[--length];
    }
    return written;
}

static uint64_t read_unsigned_arg(va_list args, int length_modifier) {
    if (length_modifier == 2) return va_arg(args, unsigned long long);
    if (length_modifier == 1) return va_arg(args, unsigned long);
    if (length_modifier == 3) return va_arg(args, size_t);
    return va_arg(args, unsigned int);
}

static int64_t read_signed_arg(va_list args, int length_modifier) {
    if (length_modifier == 2) return va_arg(args, long long);
    if (length_modifier == 1) return va_arg(args, long);
    if (length_modifier == 3) return va_arg(args, ptrdiff_t);
    return va_arg(args, int);
}

static void format_number(format_output_t *out, uint64_t value, bool negative, unsigned int base, bool uppercase, int width, bool left_align, char padding) {
    char digits[64];
    size_t digits_length = format_unsigned(digits, sizeof(digits), value, base, uppercase);
    int printed_length = (int)digits_length + (negative ? 1 : 0);

    if (!left_align && padding == '0' && negative) {
        format_putc(out, '-');
        negative = false;
    }
    if (!left_align) format_repeat(out, padding, width - printed_length);
    if (negative) format_putc(out, '-');
    for (size_t i = 0; i < digits_length; i++) format_putc(out, digits[i]);
    if (left_align) format_repeat(out, ' ', width - printed_length);
}

static void evict_oldest_record(void) {
    if (!record_count) return;

    log_record_t *record = &log_records[record_head];
    text_head = (record->offset + record->length) % LOG_TEXT_SIZE;
    text_used -= record->length;
    first_text_sequence += record->length;
    record_head = (record_head + 1) % LOG_RECORD_COUNT;
    record_count--;
    dropped_records++;
}

static void append_record(const char *message, size_t length) {
    if (!length) return;
    if (length > LOG_TEXT_SIZE) {
        message += length - LOG_TEXT_SIZE;
        length = LOG_TEXT_SIZE;
    }

    while (record_count && (LOG_TEXT_SIZE - text_used < length || record_count == LOG_RECORD_COUNT)) {
        evict_oldest_record();
    }

    size_t offset = (text_head + text_used) % LOG_TEXT_SIZE;
    size_t first_part = LOG_TEXT_SIZE - offset;
    if (first_part > length) first_part = length;
    memcpy(log_text + offset, message, first_part);
    if (first_part < length) {
        memcpy(log_text, message + first_part, length - first_part);
    }

    size_t record_index = (record_head + record_count) % LOG_RECORD_COUNT;
    log_records[record_index] = (log_record_t) {
        .sequence = next_sequence++, .offset = offset, .length = length, };
    record_count++;
    text_used += length;
    next_text_sequence += length;
}

static void copy_log_bytes(char *buf, uint64_t sequence, size_t size) {
    size_t offset = (text_head + (size_t)(sequence - first_text_sequence)) % LOG_TEXT_SIZE;
    size_t first_part = LOG_TEXT_SIZE - offset;
    if (first_part > size) first_part = size;
    memcpy(buf, log_text + offset, first_part);
    if (first_part < size) memcpy(buf + first_part, log_text, size - first_part);
}

static void init_log_console(void) {
    if (is_console_initialized) return;
    console_level = has_boot_arg("quiet") ? 4 : 7;
    saved_console_level = console_level;
    is_console_initialized = true;
}

size_t read_log(char *buf, size_t size) {
    if (!buf || !size) return 0;

    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    uint64_t start = clear_sequence > first_text_sequence ? clear_sequence : first_text_sequence;
    size_t available = (size_t)(next_text_sequence - start);
    size_t written = size < available ? size : available;
    start = next_text_sequence - written;
    copy_log_bytes(buf, start, written);
    spin_unlock_irqrestore(&log_lock, flags);
    return written;
}

size_t read_clear_log(char *buf, size_t size) {
    if (!buf || !size) return 0;

    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    uint64_t start = clear_sequence > first_text_sequence ? clear_sequence : first_text_sequence;
    size_t available = (size_t)(next_text_sequence - start);
    size_t written = size < available ? size : available;
    start = next_text_sequence - written;
    copy_log_bytes(buf, start, written);
    clear_sequence = next_text_sequence;
    spin_unlock_irqrestore(&log_lock, flags);
    return written;
}

size_t read_stream_log(char *buf, size_t size) {
    if (!buf || !size) return 0;

    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    uint64_t start = stream_read_sequence > first_text_sequence ? stream_read_sequence : first_text_sequence;
    size_t available = (size_t)(next_text_sequence - start);
    size_t written = size < available ? size : available;
    copy_log_bytes(buf, start, written);
    stream_read_sequence = start + written;
    spin_unlock_irqrestore(&log_lock, flags);
    return written;
}

void clear_log(void) {
    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    clear_sequence = next_text_sequence;
    spin_unlock_irqrestore(&log_lock, flags);
}

size_t get_log_size(void) {
    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    uint64_t start = stream_read_sequence > first_text_sequence ? stream_read_sequence : first_text_sequence;
    size_t size = (size_t)(next_text_sequence - start);
    spin_unlock_irqrestore(&log_lock, flags);
    return size;
}

size_t get_log_capacity(void) {
    return LOG_TEXT_SIZE;
}

void control_log_console(int action, int level) {
    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    init_log_console();
    if (action == SYSLOG_ACTION_CONSOLE_OFF) {
        if (console_level) saved_console_level = console_level;
        console_level = 0;
    } else if (action == SYSLOG_ACTION_CONSOLE_ON) {
        console_level = saved_console_level;
    } else if (action == SYSLOG_ACTION_CONSOLE_LEVEL) {
        console_level = level;
    }
    spin_unlock_irqrestore(&log_lock, flags);
}

int vlog(const char *fmt, va_list args) {
    char message[LOG_MESSAGE_SIZE];
    format_output_t out = { .buf = message, .capacity = sizeof(message), .stored = 0, .total = 0 };

    if (!fmt) fmt = "(null)";
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            format_putc(&out, *p);
            continue;
        }

        p++;
        if (!*p) {
            format_putc(&out, '%');
            break;
        }

        bool left_align = false;
        char padding = ' ';
        int width = 0;
        int length_modifier = 0;

        if (*p == '-') {
            left_align = true;
            p++;
        }
        if (*p == '0') {
            if (!left_align) padding = '0';
            p++;
        }
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
                const char *string = va_arg(args, const char *);
                if (!string) string = "(null)";
                int length = (int)strlen(string);
                if (!left_align) format_repeat(&out, ' ', width - length);
                while (*string) format_putc(&out, *string++);
                if (left_align) format_repeat(&out, ' ', width - length);
                break;
            }
            case 'c':
                format_putc(&out, (char)va_arg(args, int));
                break;
            case 'd':
            case 'i': {
                int64_t signed_value = read_signed_arg(args, length_modifier);
                bool negative = signed_value < 0;
                uint64_t value = negative ? (uint64_t)(-(signed_value + 1)) + 1 : (uint64_t)signed_value;
                format_number(&out, value, negative, 10, false, width, left_align, padding);
                break;
            }
            case 'u':
                format_number(&out, read_unsigned_arg(args, length_modifier), false, 10, false, width, left_align, padding);
                break;
            case 'o':
                format_number(&out, read_unsigned_arg(args, length_modifier), false, 8, false, width, left_align, padding);
                break;
            case 'x':
            case 'X':
                format_number(&out, read_unsigned_arg(args, length_modifier), false, 16, *p == 'X', width, left_align, padding);
                break;
            case 'p': {
                uint64_t value = (uint64_t)(uintptr_t)va_arg(args, void *);
                char digits[64];
                size_t length = format_unsigned(digits, sizeof(digits), value, 16, false);
                format_putc(&out, '0');
                format_putc(&out, 'x');
                format_repeat(&out, '0', 16 - (int)length);
                for (size_t i = 0; i < length; i++) format_putc(&out, digits[i]);
                break;
            }
            case '%':
                format_putc(&out, '%');
                break;
            default:
                format_putc(&out, '%');
                format_putc(&out, *p);
                break;
        }
    }
    if (out.total >= out.capacity) {
        static const char marker[] = "<truncated>";
        size_t marker_length = sizeof(marker) - 1;
        size_t available = out.capacity - 1;
        size_t copy_length = marker_length < available ? marker_length : available;
        memcpy(message + available - copy_length, marker + marker_length - copy_length, copy_length);
        out.stored = available;
    }
    message[out.stored] = '\0';
    size_t length = out.stored;

    uint64_t flags;
    spin_lock_irqsave(&log_lock, &flags);
    append_record(message, length);
    init_log_console();
    bool print_to_console = console_level > LOG_DEFAULT_LEVEL;
    spin_unlock_irqrestore(&log_lock, flags);

    if (print_to_console && length) printf("%s", message);
    return (int)length;
}

int log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vlog(fmt, args);
    va_end(args);
    return result;
}
