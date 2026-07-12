#include <freestanding/stdint.h>
#include <io/io.h>
#include <io/rtc.h>
#include <main/timekeeping.h>
#include <main/log.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_REG_SECONDS 0x00
#define RTC_REG_MINUTES 0x02
#define RTC_REG_HOURS 0x04
#define RTC_REG_WEEKDAY 0x06
#define RTC_REG_DAY 0x07
#define RTC_REG_MONTH 0x08
#define RTC_REG_YEAR 0x09
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} rtc_time_t;

static uint8_t read_rtc_register(uint8_t reg) {
    outb(CMOS_ADDR, reg | 0x80);
    io_wait();
    return inb(CMOS_DATA);
}

static bool rtc_update_in_progress(void) {
    return (read_rtc_register(RTC_REG_STATUS_A) & 0x80) != 0;
}

static uint8_t bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0F) + ((value / 16) * 10));
}

static bool read_rtc_stable_time(rtc_time_t *time, uint8_t *status_b) {
    rtc_time_t prev;
    rtc_time_t cur;

    while (rtc_update_in_progress()) {
        __asm__ volatile("pause");
    }

    prev.second = read_rtc_register(RTC_REG_SECONDS);
    prev.minute = read_rtc_register(RTC_REG_MINUTES);
    prev.hour = read_rtc_register(RTC_REG_HOURS);
    prev.day = read_rtc_register(RTC_REG_DAY);
    prev.month = read_rtc_register(RTC_REG_MONTH);
    prev.year = read_rtc_register(RTC_REG_YEAR);

    do {
        cur = prev;

        while (rtc_update_in_progress()) {
            __asm__ volatile("pause");
        }

        prev.second = read_rtc_register(RTC_REG_SECONDS);
        prev.minute = read_rtc_register(RTC_REG_MINUTES);
        prev.hour = read_rtc_register(RTC_REG_HOURS);
        prev.day = read_rtc_register(RTC_REG_DAY);
        prev.month = read_rtc_register(RTC_REG_MONTH);
        prev.year = read_rtc_register(RTC_REG_YEAR);
    } while (cur.second != prev.second || cur.minute != prev.minute ||
             cur.hour != prev.hour || cur.day != prev.day ||
             cur.month != prev.month || cur.year != prev.year);

    *status_b = read_rtc_register(RTC_REG_STATUS_B);
    *time = cur;
    return true;
}

static bool is_leap_year(uint64_t year) {
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static uint64_t days_before_year(uint64_t year) {
    uint64_t y = year - 1;
    return (365 * y) + (y / 4) - (y / 100) + (y / 400);
}

static uint64_t days_before_month(uint64_t year, uint8_t month) {
    static const uint16_t month_days[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    uint64_t days = month_days[month - 1];
    if (month > 2 && is_leap_year(year)) {
        days++;
    }

    return days;
}

static uint64_t rtc_to_unix_seconds(const rtc_time_t *time) {
    uint64_t year = time->year;
    year += year < 70 ? 2000 : 1900;

    uint64_t days = days_before_year(year) - days_before_year(1970);
    days += days_before_month(year, time->month);
    days += time->day - 1;

    return (days * 86400ULL) +
           ((uint64_t)time->hour * 3600ULL) +
           ((uint64_t)time->minute * 60ULL) +
           time->second;
}

uint64_t read_rtc_unix_time(void) {
    rtc_time_t time;
    uint8_t status_b;

    if (!read_rtc_stable_time(&time, &status_b)) {
        return 0;
    }

    if (!(status_b & 0x04)) {
        time.second = bcd_to_binary(time.second);
        time.minute = bcd_to_binary(time.minute);
        time.hour = (uint8_t)((time.hour & 0x80) | bcd_to_binary(time.hour & 0x7F));
        time.day = bcd_to_binary(time.day);
        time.month = bcd_to_binary(time.month);
        time.year = bcd_to_binary(time.year);
    }

    if (!(status_b & 0x02) && (time.hour & 0x80)) {
        time.hour = (uint8_t)(((time.hour & 0x7F) + 12) % 24);
    }

    if (time.month < 1 || time.month > 12 || time.day < 1 || time.day > 31 ||
        time.hour > 23 || time.minute > 59 || time.second > 59) {
        return 0;
    }

    return rtc_to_unix_seconds(&time);
}

void init_rtc(void) {
    uint64_t unix_seconds = read_rtc_unix_time();
    if (!unix_seconds) {
        log("failed to read rtc");
        return;
    }

    time_seed_realtime_us(unix_seconds * 1000000ULL);
    log("initialized rtc");
}
