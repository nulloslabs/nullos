#include <main/limine_req.h>
#include <main/string.h>
#include <main/strings.h>
#include <io/serial.h>
#include <io/terminal.h>

const char *get_boot_args(void) {
    if (cmdline_req.response == NULL || cmdline_req.response->executable_file == NULL) {
        return NULL;
    }

    char *args = cmdline_req.response->executable_file->string;
    if (!args || (uintptr_t)args < 0xffff000000000000ULL)
        return NULL;
    else if (args && *args) {
        return args;
    }
    return NULL;
}

const char* get_arg_value(const char* args, const char* key) {
    if (!args || !key) return NULL;
    
    size_t key_len = strlen(key);
    const char* s = args;

    while (s) {
        // Check if the key matches AND is either at start or after a space
        if ((s == args || *(s - 1) == ' ') && strncmp(s, key, key_len) == 0 && s[key_len] == '=') {
            return s + key_len + 1; // Return the value after the '='
        }
        
        // Move to the next space to check the next argument
        s = strchr(s, ' ');
        if (s) s++; // Skip the space itself
    }
    return NULL;
}

void parse_boot_args(void) {
    const char *args = get_boot_args();
    if (!args) {
        printf("Boot Arguments: Parsed boot arguments.\n");
        return;
    }
    const char *val = get_arg_value(args, "serial");
    
    if (val) {
        if (strncasecmp(val, "COM1", 4) == 0) g_debug_port = COM1;
        else if (strncasecmp(val, "COM2", 4) == 0) g_debug_port = COM2;
        else if (strncasecmp(val, "COM3", 4) == 0) g_debug_port = COM3;
        else if (strncasecmp(val, "COM4", 4) == 0) g_debug_port = COM4;
    }
    printf("Boot Arguments: Parsed boot arguments.\n");
}