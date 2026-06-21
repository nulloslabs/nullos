#include <freestanding/sys/utsname.h>
#include <main/uname.h>
#include <main/string.h>
#include <io/terminal.h>

struct utsname uname_info;

void cache_uname(void) {
    // I am too lazy to make more #define's so like...just hardcode ._.
    strcpy(uname_info.sysname, "NullOS");
    strcpy(uname_info.nodename, "(none)"); // Hardcode (none), syscalls already fill it in
    strcpy(uname_info.release, "1.0.0");
    strcpy(uname_info.version, __DATE__ " " __TIME__);
    strcpy(uname_info.machine, "x86_64");
    strcpy(uname_info.domainname, "");
    printf("uname: cached uname\n");
}
