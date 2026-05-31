#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define UTSNAME_LENGTH 65

struct utsname {
    char sysname[UTSNAME_LENGTH];
    char nodename[UTSNAME_LENGTH];
    char release[UTSNAME_LENGTH];
    char version[UTSNAME_LENGTH];
    char machine[UTSNAME_LENGTH];
    char processor[UTSNAME_LENGTH];
    char hardware_platform[UTSNAME_LENGTH];
    char operating_system[UTSNAME_LENGTH];
};

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif
