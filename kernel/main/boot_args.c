#include <freestanding/stdint.h>
#include <main/boot_args.h>
#include <main/limine_req.h>
#include <main/string.h>
#include <main/strings.h>
#include <main/log.h>
#include <io/serial.h>

static const char *args = NULL;

const char *get_boot_args(void) {
    if (cmdline_req.response == NULL || cmdline_req.response->executable_file == NULL) return NULL;
    char *raw_args = cmdline_req.response->executable_file->string;
    if (!raw_args || (uintptr_t)raw_args < 0xffff000000000000ULL) return NULL;
    if (*raw_args) return raw_args;
    return NULL;
}

const char *get_arg_value(const char *key) {
    if (!key || !args) return NULL;

    size_t key_len = strlen(key);
    const char *s = args;
    while (*s) {
        // Check if the key matches AND is either at start or after a space
        if ((s == args || *(s - 1) == ' ') && strncmp(s, key, key_len) == 0 && s[key_len] == '=') return s + key_len + 1; // Return the value after the '='
        // Move to the next space to check the next argument
        s = strchr(s, ' ');
        if (s) s++; // Skip the space itself
    }
    return NULL;
}

void parse_boot_args(void) {
    args = get_boot_args();
    // NOTE: If we need anything, we can just put if checks here, right now it dosen't really parse anything...xD
    log("parsed boot args");
}
