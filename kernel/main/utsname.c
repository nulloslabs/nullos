#include <freestanding/sys/utsname.h>
#include <main/utsname.h>
#include <main/string.h>
#include <io/terminal.h>

struct utsname utsname_info;

void cache_utsname(void) {
    // I am too lazy to make more #define's so like...just hardcode ._.
    strcpy(utsname_info.sysname, "NullOS");
    strcpy(utsname_info.nodename, "(none)"); // Hardcode (none), syscalls already fill it in
    strcpy(utsname_info.release, "1.0.0");
    strcpy(utsname_info.version, __DATE__ " " __TIME__);
    strcpy(utsname_info.machine, "x86_64");
    strcpy(utsname_info.domainname, "");
    printf("utsname: cached utsname\n");
}
