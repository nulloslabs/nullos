#include <sys/utsname.h>
#include <main/kernel.h>
#include <main/log.h>
#include <sys/domainname.h>
#include <sys/hostname.h>
#include <util/string.h>

struct utsname utsname;

void cache_utsname(void) {
    strlcpy(utsname.sysname, KERNEL_SYSNAME, sizeof(utsname.sysname));
    strlcpy(utsname.nodename, HOSTNAME_DEFAULT, sizeof(utsname.nodename));
    strlcpy(utsname.release, KERNEL_RELEASE, sizeof(utsname.release));
    strlcpy(utsname.version, __DATE__ " " __TIME__, sizeof(utsname.version));
    strlcpy(utsname.machine, "x86_64", sizeof(utsname.machine));
    strlcpy(utsname.domainname, DEFAULT_DOMAINNAME, sizeof(utsname.domainname)); // Same thing as nodename, hardcode (none)
    log("utsname: cached utsname\n");
}
