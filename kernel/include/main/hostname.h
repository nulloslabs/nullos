#pragma once

#define HOSTNAME_DEFAULT "(none)"
#define HOSTNAME_MAX_LEN 65

int get_hostname(char *name, size_t len);
int set_hostname(const char *name, size_t len);
