#include <stdint.h>
#include <stdbool.h>
#include <main/boot_args.h>
#include <main/limine_req.h>
#include <main/string.h>
#include <main/strings.h>

const char *get_boot_args(void) {
    if (cmdline_req.response == NULL || cmdline_req.response->executable_file == NULL) return NULL;
    char *raw_args = cmdline_req.response->executable_file->string;
    if (!raw_args || (uintptr_t)raw_args < 0xffff000000000000ULL) return NULL;
    if (*raw_args) return raw_args;
    return NULL;
}

bool has_boot_arg(const char *key) {
    const char *args = get_boot_args();
    if (!key || !*key || !args) return false;

    size_t key_len = strlen(key);
    const char *s = args;
    while (*s) {
        while (*s == ' ') s++;
        if (strncmp(s, key, key_len) == 0 && (s[key_len] == '\0' || s[key_len] == ' ')) return true;
        while (*s && *s != ' ') s++;
    }
    return false;
}

const char *get_arg_value(const char *key) {
    const char *args = get_boot_args();
    if (!key || !args) return NULL;

    size_t key_len = strlen(key);
    const char *s = args;
    while (*s) {
        if ((s == args || *(s - 1) == ' ') && strncmp(s, key, key_len) == 0 && s[key_len] == '=') return s + key_len + 1;
        s = strchr(s, ' ');
        if (!s) return NULL;
        s++;
    }
    return NULL;
}
