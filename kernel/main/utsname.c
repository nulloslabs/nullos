#include <freestanding/sys/utsname.h>
#include <main/utsname.h>
#include <main/kernel.h>
#include <main/string.h>
#include <main/log.h>

struct utsname utsname_info;

void cache_utsname(void) {
    strlcpy(utsname_info.sysname, KERNEL_SYSNAME, sizeof(utsname_info.sysname));
    strlcpy(utsname_info.nodename, "(none)", sizeof(utsname_info.nodename)); // Hardcode (none), syscalls already fill it in
    strlcpy(utsname_info.release, KERNEL_RELEASE, sizeof(utsname_info.release));
    strlcpy(utsname_info.version, __DATE__ " " __TIME__, sizeof(utsname_info.version));
    strlcpy(utsname_info.machine, "x86_64", sizeof(utsname_info.machine));
    strlcpy(utsname_info.domainname, "(none)", sizeof(utsname_info.domainname)); // Same thing as nodename, hardcode (none)
    log("cached utsname");
}
