#include <main/uname.h>
#include <main/string.h>
#include <main/hostname.h>
#include <io/terminal.h>

utsname_t uname_info;

void cache_uname(void) {
    char nodename[65];
    get_hostname(nodename, sizeof(nodename));

    // I am too lazy to make more #define's so like...just hardcode ._.
    strcpy(uname_info.sysname, "NullOS");
    strcpy(uname_info.nodename, nodename);
    strcpy(uname_info.release, "1.0.0");
    strcpy(uname_info.version, __DATE__ " " __TIME__);
    strcpy(uname_info.machine, "x86_64");
    strcpy(uname_info.processor, "unknown");
    strcpy(uname_info.hardware_platform, "unknown");
    strcpy(uname_info.operating_system, "NullOS");
    printf("Uname: Cached Uname.\n"); // Not my best init message, I know.
}
