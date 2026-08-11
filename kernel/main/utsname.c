#include <stdbool.h>
#include <sys/utsname.h>
#include <main/log.h>
#include <main/utsname.h>
#include <main/kernel.h>
#include <main/hostname.h>
#include <main/domainname.h>
#include <main/string.h>

struct utsname utsname;

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

void cache_utsname(void) {
    char major[21];
    char minor[21];
    char patch[21];

    strlcpy(utsname.sysname, KERNEL_SYSNAME, sizeof(utsname.sysname));
    strlcpy(utsname.nodename, HOSTNAME_DEFAULT, sizeof(utsname.nodename));

    int_to_str(KERNEL_MAJOR, major, sizeof(major), 10, false);
    int_to_str(KERNEL_MINOR, minor, sizeof(minor), 10, false);
    int_to_str(KERNEL_PATCH, patch, sizeof(patch), 10, false);

    strlcpy(utsname.release, major, sizeof(utsname.release));
    strlcat(utsname.release, ".", sizeof(utsname.release));
    strlcat(utsname.release, minor, sizeof(utsname.release));
    strlcat(utsname.release, ".", sizeof(utsname.release));
    strlcat(utsname.release, patch, sizeof(utsname.release));

    strlcpy(utsname.version, __DATE__ " " __TIME__, sizeof(utsname.version));
    strlcpy(utsname.machine, "x86_64", sizeof(utsname.machine));
    strlcpy(utsname.domainname, "(none)", sizeof(utsname.domainname)); // Same thing as nodename, hardcode (none)
    log("utsname: cached utsname\n");
}
